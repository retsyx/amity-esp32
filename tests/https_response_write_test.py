#!/usr/bin/env python3
"""Exercise real response/SDK framing and AES bounce sizing under modeled DMA pressure.

The pinned SDK functions are compiled unchanged. Only TLS encryption and the
allocator/device seams are faked; this is not a measurement of bench free heap.
--web-source permits the same assertions against a prior firmware source.
"""
import argparse
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
struct esp_tls_t{int ssl=0,error_handle=0;};
std::string wire;
unsigned tls_calls=0;
bool short_writes=false,want_next=false;
int mbedtls_ssl_write(int*,const unsigned char *data,size_t size){
    ++tls_calls;
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
unsigned chunk_calls=0,fail_chunk=0,terminators=0;
std::vector<size_t> chunks;
esp_err_t httpd_resp_send_chunk(httpd_req_t *request,const char *data,ssize_t size){
    ++chunk_calls;chunks.push_back(size);
    if(fail_chunk==chunk_calls)return ESP_ERR_HTTPD_RESP_SEND;
    if(!size)++terminators;
    return sdk_httpd_resp_send_chunk(request,data,size);
}
esp_tls_t tls;
int send_fn(void*,int,const char *data,size_t size,int){return static_cast<int>(esp_mbedtls_write(&tls,data,size));}
struct Request{
    sock_db socket;httpd_req_aux aux;httpd_data server;httpd_req_t request;
    Request(){socket.send_fn=send_fn;aux.sd=&socket;request.aux=&aux;request.handle=&server;}
};
void reset(){wire.clear();tls_calls=chunk_calls=fail_chunk=terminators=dma_failures=0;short_writes=want_next=false;largest_dma_request=dma_peak=dma_live=0;dma_sizes.clear();chunks.clear();}
std::string payload(size_t size){std::string data(size,'x');for(size_t i=0;i<size;++i)data[i]=static_cast<char>('a'+i%26);return data;}
std::string decode(){
    const auto split=wire.find("\r\n\r\n");check(split!=std::string::npos,"missing HTTP headers");
    if(wire.find("Transfer-Encoding: chunked\r\n")==std::string::npos)return wire.substr(split+4);
    check(wire.find("Content-Length:")==std::string::npos,"response advertised conflicting framing");
    std::string decoded;size_t offset=split+4;
    for(;;){const auto end=wire.find("\r\n",offset);check(end!=std::string::npos,"missing chunk length");
        const auto count=std::stoul(wire.substr(offset,end-offset),nullptr,16);offset=end+2;
        check(offset+count+2<=wire.size(),"truncated body chunk");decoded.append(wire,offset,count);offset+=count;
        check(wire.substr(offset,2)=="\r\n","missing chunk delimiter");offset+=2;
        if(!count){check(offset==wire.size(),"bytes after terminal chunk");return decoded;}}
}
'''

SUITE = r'''
int main(){try{
    // The real SDK path reproduces a large record's allocation failure when
    // the modeled largest available DMA block is 1024 bytes. No heap numbers
    // from this fixture are claimed to describe the physical failure instant.
    reset();Request old;const auto large=payload(6218);
    check(httpd_resp_send(&old.request,large.data(),large.size())!=ESP_OK,"legacy whole-body path did not expose DMA pressure");
    check(dma_failures==1&&largest_dma_request==1600,"failure did not use real SDK AES bounce sizing");
    std::cout<<"Reproduced SDK whole-body failure: 6218-byte body requests 1600-byte DMA bounce with modeled 1024-byte block limit\n";
    for(size_t size:{size_t{6218},size_t{0},size_t{1},size_t{512},size_t{513},size_t{40000}}){
        reset();Request current;const auto data=payload(size);
        check(web::json(&current.request,data,"202 Accepted")==ESP_OK,"bounded response failed under modeled pressure");
        check(decode()==data&&dma_live==0,"HTTP body was corrupted or DMA memory retained");
        check(largest_dma_request<=512&&dma_peak<=1024,"application write exceeded the DMA bound");
        check(wire.find("HTTP/1.1 202 Accepted\r\n")==0&&wire.find("Content-Type: application/json\r\n")!=std::string::npos,"status/content type changed");
        check(wire.find("Cache-Control: no-store\r\n")!=std::string::npos&&wire.find("X-Content-Type-Options: nosniff\r\n")!=std::string::npos,"security/cache headers changed");
        check((size>512?terminators==1&&chunk_calls==(size+511)/512+1:chunk_calls==0),"small/large response framing changed");
        if(size<=512)check(wire.find("Content-Length: "+std::to_string(size)+"\r\n")!=std::string::npos,"small response length changed");
    }
    reset();Request partial;short_writes=true;
    check(web::json(&partial.request,large,"200 OK")==ESP_OK&&decode()==large&&terminators==1,"SDK positive short/WANT writes lost bytes");
    for(unsigned failure:{1u,2u,4u}){
        reset();Request failed;fail_chunk=failure;
        check(web::json(&failed.request,payload(1300),"200 OK")==ESP_ERR_HTTPD_RESP_SEND,"failed chunk reported success");
        check(chunk_calls==failure&&terminators==0,"failure was retried or response falsely terminated");
    }
    reset();Request hard;dma_limit=128;
    check(web::json(&hard.request,large,"200 OK")!=ESP_OK&&dma_failures==1&&chunk_calls==1&&terminators==0,"hard allocator error was retried/hidden");
    check(dma_live==0,"failed DMA allocation retained memory");
    std::cout<<"PASS real web/SDK HTTP framing, SDK partial writes and AES sizing: exact empty/512/513/large bodies; first/middle/final failures propagate; bounded app records preserve 4096-byte TLS handshake capacity\n";
    return 0;
}catch(const std::exception &error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--web-source', type=Path, default=ROOT / 'components/web/request.cpp')
    args = parser.parse_args()
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
    web = args.web_source.read_text()
    txrx = (IDF / 'components/esp_http_server/src/httpd_txrx.c').read_text()
    tls = (IDF / 'components/esp-tls/esp_tls_mbedtls.c').read_text()
    aes = (IDF / 'components/mbedtls/port/aes/dma/esp_aes_dma_core.c').read_text()
    actual_aes = function(aes, 'static int esp_aes_process_dma_ext_ram(')
    # The SDK is C; separate adjacent format-macro tokens for the C++ fixture.
    actual_tls = function(tls, 'ssize_t esp_mbedtls_write(').replace('"NEWLIB_', '" NEWLIB_')
    actual_http = ''.join(function(txrx, name) for name in [
        'static esp_err_t httpd_send_all(', 'esp_err_t httpd_resp_send(', 'esp_err_t httpd_resp_send_chunk('])
    # Name the real SDK chunk function separately only to inject its documented
    # failure result at first/middle/final calls of the real application helper.
    actual_http = actual_http.replace('esp_err_t httpd_resp_send_chunk(', 'esp_err_t sdk_httpd_resp_send_chunk(')
    actual_web = ('namespace web {\n'
                  + (function(web, 'esp_err_t response(httpd_req_t *req,') if 'esp_err_t response(' in web else '')
                  + function(web, 'esp_err_t json(httpd_req_t *req,') + '}\n')
    source = (PREFIX + '\n#define free dma_free\n' + actual_aes + '\n#undef free\n'
              + MIDDLE + actual_tls + '\n#define malloc http_alloc\n' + actual_http
              + '\n#undef malloc\n' + SUFFIX + actual_web + SUITE)
    with tempfile.TemporaryDirectory(prefix='amity-https-write-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(source)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
