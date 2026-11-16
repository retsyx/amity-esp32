#!/usr/bin/env python3
"""Run the actual port-80 redirect server setup and handler against HTTPD seams.

Only the HTTPD API is faked. The SDK defaults the redirect relies on, and the
LWIP socket pool shared by every listener, are checked against the sources.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))

HTTPD = r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sys/types.h>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NOT_FOUND=0x105;
#define MALLOC_CAP_8BIT 4u
#define MALLOC_CAP_SPIRAM 1024u
#define MALLOC_CAP_INTERNAL 2048u
using httpd_handle_t=void*;
enum httpd_method_t {HTTP_DELETE,HTTP_GET,HTTP_HEAD,HTTP_POST,HTTP_PUT,HTTP_OPTIONS=6,HTTP_PATCH=28};
struct httpd_req_t {httpd_handle_t handle;int method;const char *uri;};
using httpd_uri_match_func_t=bool(*)(const char*,const char*,size_t);
struct httpd_config_t {
    unsigned task_priority;size_t stack_size;uint32_t task_caps;
    uint16_t server_port,ctrl_port,max_open_sockets,max_uri_handlers;
    bool lru_purge_enable;uint16_t recv_wait_timeout,send_wait_timeout;
    httpd_uri_match_func_t uri_match_fn;
};
// Mirrors the pinned SDK defaults; main() checks those against the SDK header.
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{5,4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,80,32768,7,8,false,5,5,nullptr}
bool httpd_uri_match_wildcard(const char*,const char*,size_t);
esp_err_t httpd_start(httpd_handle_t*,const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t*,const char*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_send(httpd_req_t*,const char*,ssize_t);
int httpd_req_to_sockfd(httpd_req_t*);
esp_err_t httpd_sess_trigger_close(httpd_handle_t,int);
'''

TIMER = 'using esp_timer_handle_t=void*;\n'

SUITE = r'''
#include "web.hpp"
#include <cassert>
#include <iostream>
#include <map>
using namespace std::string_literals;
// HTTPD server and request seams.
httpd_config_t captured;
httpd_handle_t const server=reinterpret_cast<void*>(0x80);
int start_result=ESP_OK,route_result=ESP_OK,route_fail_at=-1;
unsigned starts=0,stops=0;
std::vector<int> methods;
web::Handler handler=nullptr;
bool httpd_uri_match_wildcard(const char*,const char*,size_t){return true;}
esp_err_t httpd_start(httpd_handle_t *handle,const httpd_config_t *config){
    ++starts;captured=*config;if(start_result==ESP_OK)*handle=server;return start_result;
}
esp_err_t httpd_stop(httpd_handle_t handle){assert(handle==server);++stops;return ESP_OK;}
namespace web {
esp_err_t response(httpd_req_t *request,const char *data,size_t size){return httpd_resp_send(request,data,size);}
esp_err_t route(httpd_handle_t handle,const char *path,httpd_method_t method,Handler h,void *context){
    assert(handle==server&&!std::strcmp(path,"/*")&&!context);
    if(static_cast<int>(methods.size())==route_fail_at)return route_result;
    assert(!handler||handler==h);handler=h;methods.push_back(method);return ESP_OK;
}
}
const char *host_header=nullptr;
std::string status,sent;
std::vector<std::pair<std::string,const char*>> headers;
std::vector<std::string> events;
size_t httpd_req_get_hdr_value_len(httpd_req_t*,const char *field){
    assert(!std::strcmp(field,"Host"));return host_header?std::strlen(host_header):0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char *field,char *value,size_t size){
    assert(!std::strcmp(field,"Host")&&host_header&&size==std::strlen(host_header)+1);
    std::memcpy(value,host_header,size);return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t*,const char *value){status=value;return ESP_OK;}
// HTTPD keeps header pointers until the response is sent.
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char *field,const char *value){headers.emplace_back(field,value);return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t*,const char *body,ssize_t size){
    assert(!body&&!size);
    sent="HTTP/1.1 "+status+"\r\n";for(const auto &h:headers)sent+=h.first+": "+h.second+"\r\n";
    events.push_back("send");return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *request){assert(request->handle==server);return 54;}
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle,int socket){
    assert(handle==server&&socket==54);events.push_back("close");return ESP_OK;
}
std::string serve(const char *method_uri,const char *host){
    host_header=host;status.clear();sent.clear();headers.clear();events.clear();
    httpd_req_t request{server,HTTP_GET,method_uri};
    assert(handler(&request)==ESP_OK);
    // Every response closes its connection after it has been sent.
    assert((events==std::vector<std::string>{"send","close"}));
    return sent;
}
int main(){
    // The requested URL is unchanged apart from its scheme.
    assert(web::https_location("amity.local","/")=="https://amity.local/");
    assert(web::https_location("amity.local","/api/status?view=all&x=%20")=="https://amity.local/api/status?view=all&x=%20");
    assert(web::https_location("Living-Room.local:8080","/a/b/")=="https://Living-Room.local:8080/a/b/");
    assert(web::https_location("10.0.1.7","/app.js")=="https://10.0.1.7/app.js");
    assert(web::https_location("[fe80::1]","/")=="https://[fe80::1]/");
    // An absolute-form target already names the host.
    assert(web::https_location("other.local","http://amity.local/x?y=1")=="https://amity.local/x?y=1");
    assert(web::https_location("","HTTP://Amity.local/")=="https://Amity.local/");
    // Without a host, or with a target that is not a path, no URL can be formed.
    assert(web::https_location("","/").empty());
    assert(web::https_location("amity.local","").empty());
    assert(web::https_location("amity.local","*").empty());
    assert(web::https_location("amity.local","https://amity.local/").empty());

    httpd_handle_t handle=nullptr;
    assert(web::start_redirect(handle)==ESP_OK&&handle==server&&starts==1&&stops==0);
    assert(captured.server_port==80&&captured.ctrl_port==32768);
    assert(captured.max_open_sockets==2&&captured.lru_purge_enable);
    assert(captured.max_uri_handlers==methods.size()&&captured.uri_match_fn==httpd_uri_match_wildcard);
    assert(captured.task_caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)&&captured.stack_size==4096);
    assert((methods==std::vector<int>{HTTP_GET,HTTP_HEAD,HTTP_POST,HTTP_PUT,HTTP_DELETE,HTTP_PATCH,HTTP_OPTIONS}));

    assert(serve("/","amity.local")=="HTTP/1.1 307 Temporary Redirect\r\nLocation: https://amity.local/\r\nConnection: close\r\n");
    assert(serve("/api/logs?since=12","living-room.local")=="HTTP/1.1 307 Temporary Redirect\r\nLocation: https://living-room.local/api/logs?since=12\r\nConnection: close\r\n");
    assert(serve("http://amity.local/style.css",nullptr)=="HTTP/1.1 307 Temporary Redirect\r\nLocation: https://amity.local/style.css\r\nConnection: close\r\n");
    assert(serve("/",nullptr)=="HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");

    // A failed registration stops the partly configured server.
    handle=nullptr;methods.clear();handler=nullptr;route_fail_at=3;route_result=0x103;
    assert(web::start_redirect(handle)==0x103&&!handle&&starts==2&&stops==1&&methods.size()==3);
    handle=nullptr;methods.clear();route_fail_at=-1;start_result=0x101;
    assert(web::start_redirect(handle)==0x101&&!handle&&starts==3&&stops==1&&methods.empty());
    std::cout<<"PASS actual HTTP redirect: port 80, same URL over HTTPS, 307 for every routed method, connection closed after each response\n";
}
'''


def config_value(text, name):
    match = re.search(r'^' + re.escape(name) + r'=(\d+)$', text, re.M)
    assert match, name
    return int(match.group(1))


def check_sdk_defaults():
    header = (IDF / 'components/esp_http_server/include/esp_http_server.h').read_text()
    default = header[header.index('#define HTTPD_DEFAULT_CONFIG()'):]
    default = default[:default.index('}')]
    fields = dict(re.findall(r'\.(\w+)\s*=\s*([^,]+?)\s*,', default))
    assert fields['server_port'] == '80' and fields['ctrl_port'] == 'ESP_HTTPD_DEF_CTRL_PORT', fields
    assert fields['stack_size'] == '4096', fields
    assert re.search(r'#define ESP_HTTPD_DEF_CTRL_PORT\s+\(32768\)', header)
    ssl = (IDF / 'components/esp_https_server/include/esp_https_server.h').read_text()
    assert '.ctrl_port   = ESP_HTTPD_DEF_CTRL_PORT+1' in ssl
    assert '.port_secure = 443' in ssl


def check_socket_pool():
    # Each HTTPD holds its listener and two control sockets besides sessions.
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    https = int(re.search(r'cfg\.httpd\.max_open_sockets=(\d+);',
                          (ROOT / 'components/web/web.cpp').read_text()).group(1))
    redirect = int(re.search(r'cfg\.max_open_sockets=(\d+);',
                             (ROOT / 'components/web/redirect.cpp').read_text()).group(1))
    homekit = config_value(defaults, 'CONFIG_HAP_HTTP_MAX_OPEN_SOCKETS')
    limits = (ROOT / 'components/consortium/include/amity/consortium/limits.hpp').read_text()
    peers = int(re.search(r'maximum_roster_peers=(\d+);', limits).group(1))
    pending = int(re.search(r'maximum_pending_connections=(\d+);', limits).group(1))
    needed = (https + 3) + (homekit + 3) + (redirect + 3) + (1 + peers + pending)
    available = config_value(defaults, 'CONFIG_LWIP_MAX_SOCKETS')
    assert needed <= available, (needed, available)


def main():
    check_sdk_defaults()
    check_socket_pool()
    with tempfile.TemporaryDirectory(prefix='amity-web-redirect-') as directory:
        directory = Path(directory)
        (directory / 'esp_http_server.h').write_text(HTTPD)
        (directory / 'esp_timer.h').write_text(TIMER)
        (directory / 'esp_heap_caps.h').write_text('#pragma once\n')
        cpp, binary = directory / 'test.cpp', directory / 'test'
        cpp.write_text(SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I', str(directory), '-I', str(ROOT / 'components/web/include'),
            str(ROOT / 'components/web/redirect.cpp'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
