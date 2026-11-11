#!/usr/bin/env python3
"""Exercise real HTTPD/TLS partial writes and AES sizing under modeled DMA pressure.

The pinned SDK functions are compiled unchanged. Only TLS encryption and the
allocator/device seams are faked; this is not a measurement of bench free heap.
The old chunk-framed response is retained only to reproduce its extra records.
Production linker wrapping is verified separately against the firmware ELF.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))

PREFIX = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/types.h>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=-2,ESP_ERR_HTTPD_INVALID_REQ=-3,ESP_ERR_HTTPD_RESP_HDR=-4,ESP_ERR_HTTPD_ALLOC_MEM=-5,ESP_ERR_HTTPD_RESP_SEND=-6;
constexpr int HTTPD_RESP_USE_STRLEN=-1,HTTP_SERVER_EVENT_HEADERS_SENT=1,HTTP_SERVER_EVENT_SENT_DATA=2;
constexpr int MBEDTLS_SSL_OUT_CONTENT_LEN=4096,ESP_TLS_ERR_SSL_WANT_READ=-0x6900,ESP_TLS_ERR_SSL_WANT_WRITE=-0x6880;
#define CONFIG_MBEDTLS_DYNAMIC_BUFFER 0
#define SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 0
#define AES_MAX_CHUNK_WRITE_SIZE 1600
#define MALLOC_CAP_DMA 1
#define MIN(a,b) std::min(size_t(a),size_t(b))
#define ESP_LOGE(...) do {} while(0)
#define ESP_LOGD(...) do {} while(0)
#define ESP_INT_EVENT_TRACKER_CAPTURE(...) do {} while(0)
#define NEWLIB_NANO_SIZE_T_COMPAT_FORMAT "zu"
#define NEWLIB_NANO_SSIZE_T_COMPAT_FORMAT "zx"
#define NEWLIB_NANO_SIZE_T_COMPAT_CAST(value) value
size_t dma_limit=1024,largest_dma_request=0,dma_peak=0,dma_live=0;
unsigned dma_failures=0;
std::vector<size_t> dma_sizes;
unsigned char *heap_caps_aligned_alloc(size_t,size_t size,unsigned){
    largest_dma_request=std::max(largest_dma_request,size);dma_sizes.push_back(size);
    if(size>dma_limit){++dma_failures;return nullptr;}
    auto *memory=std::malloc(size+sizeof(size_t));if(!memory)throw std::bad_alloc();
    *static_cast<size_t*>(memory)=size;dma_live+=size;dma_peak=std::max(dma_peak,dma_live);
    return reinterpret_cast<unsigned char*>(static_cast<size_t*>(memory)+1);
}
void dma_free(void *memory){if(!memory)return;auto *allocation=static_cast<size_t*>(memory)-1;dma_live-=*allocation;std::free(allocation);}
void mbedtls_platform_zeroize(void *memory,size_t size){std::memset(memory,0,size);}
struct esp_aes_context{};
int esp_aes_process_dma(esp_aes_context*,const unsigned char *input,unsigned char *output,size_t size,uint8_t*){std::memcpy(output,input,size);return 0;}
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
'''

MIDDLE = r'''
struct esp_tls_t;
using tls_writer=ssize_t (*)(esp_tls_t*,const char*,size_t);
struct esp_tls_t{int ssl=0,error_handle=0;tls_writer write=nullptr;};
std::string wire;
unsigned tls_calls=0,fail_tls_call=0,zero_tls_call=0;
std::vector<size_t> tls_records;
bool short_writes=false,want_next=false,bounded_writes=true;
int mbedtls_ssl_write(int*,const unsigned char *data,size_t size){
    ++tls_calls;tls_records.push_back(size);
    if(tls_calls==fail_tls_call)return -0x84;
    if(tls_calls==zero_tls_call)return 0;
    if(want_next){want_next=false;return ESP_TLS_ERR_SSL_WANT_WRITE;}
    std::vector<unsigned char> encrypted(size);esp_aes_context context;
    if(esp_aes_process_dma_ext_ram(&context,data,encrypted.data(),size,nullptr,true,true))return -0x84;
    const auto count=short_writes?std::min(size,size_t{7}):size;
    wire.append(reinterpret_cast<const char*>(encrypted.data()),count);want_next=short_writes&&count<size;
    return static_cast<int>(count);
}
void mbedtls_print_error_msg(ssize_t){}
struct sock_db{int fd=9;void *handle=nullptr;int (*send_fn)(void*,int,const char*,size_t,int)=nullptr;};
struct header{const char *field=nullptr,*value=nullptr;};
struct httpd_req_aux{
    const char *status="200 OK",*content_type="application/octet-stream";
    unsigned req_hdrs_count=0,resp_hdrs_count=0;size_t max_req_hdr_len=1024;
    header resp_hdrs[8]{};bool first_chunk_sent=false;sock_db *sd=nullptr;
};
struct httpd_data{int http_server_state=0;};
struct httpd_req_t{httpd_req_aux *aux=nullptr;httpd_data *handle=nullptr;};
struct esp_http_server_event_data{int fd=0;ssize_t data_len=0;};
bool httpd_valid_req(httpd_req_t *request){return request->aux&&request->handle;}
void esp_http_server_dispatch_event(int,const void*,size_t){}
char *http_alloc(size_t size){return static_cast<char*>(std::malloc(size));}
int httpd_resp_set_status(httpd_req_t *request,const char *status){static_cast<httpd_req_aux*>(request->aux)->status=status;return ESP_OK;}
int httpd_resp_set_type(httpd_req_t *request,const char *type){static_cast<httpd_req_aux*>(request->aux)->content_type=type;return ESP_OK;}
int httpd_resp_set_hdr(httpd_req_t *request,const char *name,const char *value){auto &aux=*static_cast<httpd_req_aux*>(request->aux);aux.resp_hdrs[aux.resp_hdrs_count++]={name,value};return ESP_OK;}
'''

SUFFIX = r'''
unsigned chunk_calls=0,terminators=0;
esp_err_t httpd_resp_send_chunk(httpd_req_t *request,const char *data,ssize_t size){
    ++chunk_calls;
    if(!size)++terminators;
    return sdk_httpd_resp_send_chunk(request,data,size);
}
esp_tls_t tls;
int send_fn(void*,int,const char *data,size_t size,int){
    return static_cast<int>(bounded_writes?__wrap_esp_tls_conn_write(&tls,data,size):__real_esp_tls_conn_write(&tls,data,size));
}
struct Request{
    sock_db socket;httpd_req_aux aux;httpd_data server;httpd_req_t request;
    Request(){tls.write=esp_mbedtls_write;socket.send_fn=send_fn;aux.sd=&socket;request.aux=&aux;request.handle=&server;}
};
void reset(){wire.clear();tls_calls=fail_tls_call=zero_tls_call=chunk_calls=terminators=dma_failures=0;short_writes=want_next=false;bounded_writes=true;largest_dma_request=dma_peak=dma_live=0;dma_sizes.clear();tls_records.clear();}
std::string payload(size_t size){std::string data(size,'x');for(size_t i=0;i<size;++i)data[i]=static_cast<char>('a'+i%26);return data;}
std::string decode(){
    const auto split=wire.find("\r\n\r\n");check(split!=std::string::npos,"missing HTTP headers");
    if(wire.find("Transfer-Encoding: chunked\r\n")==std::string::npos){
        const auto field=wire.find("Content-Length: ");check(field!=std::string::npos,"missing response length");
        const auto count=std::stoul(wire.substr(field+16));check(count==wire.size()-split-4,"response length does not match body");
        return wire.substr(split+4);
    }
    check(wire.find("Content-Length:")==std::string::npos,"response advertised conflicting framing");
    std::string decoded;size_t offset=split+4;
    for(;;){const auto end=wire.find("\r\n",offset);check(end!=std::string::npos,"missing chunk length");
        const auto count=std::stoul(wire.substr(offset,end-offset),nullptr,16);offset=end+2;
        check(offset+count+2<=wire.size(),"truncated body chunk");decoded.append(wire,offset,count);offset+=count;
        check(wire.substr(offset,2)=="\r\n","missing chunk delimiter");offset+=2;
        if(!count){check(offset==wire.size(),"bytes after terminal chunk");return decoded;}}
}
'''

LEGACY = r'''
// Previous bounded application response: each body chunk makes three SDK
// writes (length, body, delimiter), even though the entire body is available.
namespace legacy {
esp_err_t json(httpd_req_t *request,const std::string &data){
    httpd_resp_set_type(request,"application/json");
    httpd_resp_set_hdr(request,"Cache-Control","no-store");
    httpd_resp_set_hdr(request,"X-Content-Type-Options","nosniff");
    for(size_t offset=0;offset<data.size();){
        const auto count=std::min(size_t{512},data.size()-offset);
        const auto result=httpd_resp_send_chunk(request,data.data()+offset,count);
        if(result!=ESP_OK)return result;
        offset+=count;
    }
    return httpd_resp_send_chunk(request,nullptr,0);
}
}
'''

SUITE = r'''
int main(){try{
    // The real SDK path reproduces a large record's allocation failure when
    // the modeled largest available DMA block is 1024 bytes. No heap numbers
    // from this fixture are claimed to describe the physical failure instant.
    reset();bounded_writes=false;Request old;const auto large=payload(6218);
    check(httpd_resp_send(&old.request,large.data(),large.size())!=ESP_OK,"legacy whole-body path did not expose DMA pressure");
    check(dma_failures==1&&largest_dma_request==1600,"failure did not use real SDK AES bounce sizing");
    std::cout<<"Reproduced SDK whole-body failure: 6218-byte body requests 1600-byte DMA bounce with modeled 1024-byte block limit\n";
    reset();Request chunked;
    check(legacy::json(&chunked.request,large)==ESP_OK&&decode()==large,"old chunked response reproduction failed");
    const auto old_records=tls_calls;
    check(chunk_calls==14&&terminators==1&&old_records==51,"old chunk framing record multiplier changed");
    reset();Request normal;
    check(web::json(&normal.request,large,"200 OK")==ESP_OK&&decode()==large,"normal bounded response failed");
    const auto normal_records=tls_calls;const auto expected=wire;
    check(normal_records==23&&normal_records<old_records&&chunk_calls==0,"HTTP framing still adds tiny chunk TLS records");
    std::cout<<"Reproduced/fixed framing amplification: 6218-byte response uses "<<old_records<<" -> "<<normal_records<<" TLS application records\n";
    for(size_t size:{size_t{6218},size_t{0},size_t{1},size_t{512},size_t{513},size_t{40000}}){
        reset();Request current;const auto data=payload(size);
        check(web::json(&current.request,data,"202 Accepted")==ESP_OK,"bounded response failed under modeled pressure");
        check(decode()==data&&dma_live==0,"HTTP body was corrupted or DMA memory retained");
        check(largest_dma_request<=512&&dma_peak<=1024,"application write exceeded the DMA bound");
        check(wire.find("HTTP/1.1 202 Accepted\r\n")==0&&wire.find("Content-Type: application/json\r\n")!=std::string::npos,"status/content type changed");
        check(wire.find("Cache-Control: no-store\r\n")!=std::string::npos&&wire.find("X-Content-Type-Options: nosniff\r\n")!=std::string::npos,"security/cache headers changed");
        check(chunk_calls==0&&terminators==0&&wire.find("Transfer-Encoding:")==std::string::npos,"response used HTTP chunk framing");
        check(wire.find("Content-Length: "+std::to_string(size)+"\r\n")!=std::string::npos,"response length changed");
        check(std::all_of(tls_records.begin(),tls_records.end(),[](auto count){return count<=512;}),"TLS application record exceeded512 bytes");
    }
    reset();Request partial;short_writes=true;
    check(web::json(&partial.request,large,"200 OK")==ESP_OK&&decode()==large&&terminators==0,"SDK positive short/WANT writes lost bytes");
    check(dma_live==0&&largest_dma_request<=512&&dma_peak<=1024,"short writes lost the DMA bound");
    // HTTPD must stop on the original first/header/body/final TLS write error,
    // preserving the emitted prefix without retries or a success terminator.
    for(unsigned failure:{1u,10u,11u,14u,normal_records}){
        reset();Request failed;fail_tls_call=failure;
        check(web::json(&failed.request,large,"200 OK")==ESP_ERR_HTTPD_RESP_SEND,"failed write reported success");
        check(tls_calls==failure&&chunk_calls==0&&terminators==0,"failed write was retried or response falsely terminated");
        check(wire==expected.substr(0,wire.size())&&dma_live==0,"failure changed prior bytes or retained DMA memory");
    }
    for(unsigned stopped:{1u,11u,normal_records}){
        reset();Request closed;zero_tls_call=stopped;
        check(web::json(&closed.request,large,"200 OK")==ESP_ERR_HTTPD_RESP_SEND,"zero-progress TLS write did not close response");
        check(tls_calls==stopped&&wire==expected.substr(0,wire.size())&&dma_live==0,"zero-progress write was retried or changed emitted bytes");
    }
    reset();Request hard;dma_limit=128;
    check(web::json(&hard.request,large,"200 OK")!=ESP_OK&&dma_failures==1&&chunk_calls==0&&terminators==0,"hard allocator error was retried/hidden");
    check(dma_live==0,"failed DMA allocation retained memory");
    reset();Request wants;want_next=true;
    check(__wrap_esp_tls_conn_write(&tls,large.data(),large.size())==ESP_TLS_ERR_SSL_WANT_WRITE&&tls_calls==1&&wire.empty(),"TLS WANT result was hidden/retried");
    reset();Request invalid;
    check(__wrap_esp_tls_conn_write(nullptr,large.data(),large.size())==-1&&__wrap_esp_tls_conn_write(&tls,nullptr,1)==-1&&tls_calls==0,"TLS argument validation changed");
    check(__wrap_esp_tls_conn_write(&tls,large.data(),0)==0&&tls_calls==0,"zero-length TLS write changed");
    std::cout<<"PASS real HTTPD/TLS dispatch/partial writes and AES sizing: exact Content-Length bodies; fewer TLS records; first/middle/final errors propagate;512-byte app writes preserve full TLS capacities\n";
    return 0;
}catch(const std::exception &error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    for setting in ('CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y',
                    'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384',
                    'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096'):
        assert setting in defaults.splitlines(), 'Application bound must preserve full TLS capacities: ' + setting
    # All project-owned response bodies share this helper. SDK-generated small
    # HTTP error responses retain their normal implementation.
    for directory in ('components/web', 'components/web_admin'):
        for source in (ROOT / directory).rglob('*.cpp'):
            if source.name != 'request.cpp':
                assert 'httpd_resp_send(' not in source.read_text(), 'Unbounded application response in ' + str(source)
    web = (ROOT / 'components/web/request.cpp').read_text()
    wrapper = (ROOT / 'components/web/tls_write.cpp').read_text()
    assert '"-Wl,--wrap=esp_tls_conn_write"' in (ROOT / 'components/web/CMakeLists.txt').read_text()
    txrx = (IDF / 'components/esp_http_server/src/httpd_txrx.c').read_text()
    tls = (IDF / 'components/esp-tls/esp_tls_mbedtls.c').read_text()
    dispatch = (IDF / 'components/esp-tls/esp_tls.c').read_text()
    https = (IDF / 'components/esp_https_server/src/https_server.c').read_text()
    assert 'esp_tls_conn_write(tls, buf, buf_len)' in function(https, 'static int httpd_ssl_send(')
    aes = (IDF / 'components/mbedtls/port/aes/dma/esp_aes_dma_core.c').read_text()
    actual_aes = function(aes, 'static int esp_aes_process_dma_ext_ram(')
    # The SDK is C; separate adjacent format-macro tokens for the C++ fixture.
    actual_tls = function(tls, 'ssize_t esp_mbedtls_write(').replace('"NEWLIB_', '" NEWLIB_')
    # Apple ld cannot model GNU --wrap. Use the exact SDK public dispatch body
    # under its linker real-name seam; the firmware ELF validates interposition.
    actual_dispatch = function(dispatch, 'ssize_t esp_tls_conn_write(').replace(
        'ssize_t esp_tls_conn_write(', 'extern "C" ssize_t __real_esp_tls_conn_write(')
    actual_wrapper = function(wrapper, 'extern "C" ssize_t __wrap_esp_tls_conn_write(')
    actual_http = ''.join(function(txrx, name) for name in [
        'static esp_err_t httpd_send_all(', 'esp_err_t httpd_resp_send(', 'esp_err_t httpd_resp_send_chunk('])
    # Keep the unchanged SDK chunk sender only to reproduce prior framing.
    actual_http = actual_http.replace('esp_err_t httpd_resp_send_chunk(', 'esp_err_t sdk_httpd_resp_send_chunk(')
    actual_web = ('namespace web {\n'
                  + (function(web, 'esp_err_t response(httpd_req_t *req,') if 'esp_err_t response(' in web else '')
                  + function(web, 'esp_err_t json(httpd_req_t *req,') + '}\n')
    source = (PREFIX + '\n#define free dma_free\n' + actual_aes + '\n#undef free\n'
              + MIDDLE + actual_tls + actual_dispatch + actual_wrapper
              + '\n#define malloc http_alloc\n' + actual_http
              + '\n#undef malloc\n' + SUFFIX + actual_web + LEGACY + SUITE)
    with tempfile.TemporaryDirectory(prefix='amity-https-write-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(source)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
