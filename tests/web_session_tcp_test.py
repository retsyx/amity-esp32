#!/usr/bin/env python3
"""Execute actual HTTPS startup/pre-handshake policy against native TCP sockets.

TLS identity and the SDK handshake entry are seams; no endpoint or hardware is
opened. Socket-option success and failure use the host's real TCP API.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using esp_err_t=int;
using httpd_handle_t=void*;
constexpr int ESP_OK=0;
struct esp_tls_cfg_server_t {int marker=37;};
struct esp_tls_t {int marker=41;};
struct httpd_config_t {
    unsigned max_uri_handlers=8,stack_size=10240,max_open_sockets=4;
    bool lru_purge_enable=true;
    unsigned recv_wait_timeout=5,send_wait_timeout=5;
    bool (*uri_match_fn)(const char*,const char*,size_t)=nullptr;
    esp_err_t (*open_fn)(httpd_handle_t,int)=nullptr;
};
struct httpd_ssl_config_t {
    httpd_config_t httpd;
    const uint8_t *servercert=nullptr,*prvtkey_pem=nullptr;
    size_t servercert_len=0,prvtkey_len=0;
    bool secure_transport=true,session_tickets=false;
};
#define HTTPD_SSL_CONFIG_DEFAULT() httpd_ssl_config_t{}
bool httpd_uri_match_wildcard(const char*,const char*,size_t){return true;}
int credentials_result=ESP_OK,identity_result=ESP_OK,start_result=ESP_OK;
unsigned credentials_calls=0,identity_calls=0,start_calls=0;
std::vector<std::string> warnings;
void warning(const char *tag,const char *format,int error) {
    assert(!std::strcmp(tag,"web"));
    char message[128];const int size=std::snprintf(message,sizeof(message),format,error);
    assert(size>0&&size<static_cast<int>(sizeof(message)));
    warnings.emplace_back(message);
}
#define ESP_LOGW(...) warning(__VA_ARGS__)
httpd_ssl_config_t captured;
int httpd_ssl_start(httpd_handle_t *handle,const httpd_ssl_config_t *config) {
    ++start_calls;captured=*config;
    if(start_result==ESP_OK)*handle=reinterpret_cast<void*>(1);
    return start_result;
}
namespace web {
std::string certificate,private_key;
int credentials_init(const std::string &authentication) {
    ++credentials_calls;assert(authentication=="fixture-authentication");return credentials_result;
}
int tls_identity(const std::string &hostname) {
    ++identity_calls;assert(hostname=="amity-test");
    certificate="fixture-certificate";private_key="fixture-private-key";return identity_result;
}
'''

SDK = r'''
}
int nodelay(int socket) {
    int value=-1;socklen_t size=sizeof(value);
    assert(!getsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&value,&size));
    assert(size==sizeof(value));return value;
}
struct Socket {
    int fd=::socket(AF_INET,SOCK_STREAM,0);
    Socket(){assert(fd>=0);}
    ~Socket(){::close(fd);}
};
esp_tls_cfg_server_t *expected_cfg=nullptr;
esp_tls_t *expected_tls=nullptr;
int expected_socket=-1,sdk_result=0;
unsigned sdk_calls=0;
extern "C" int __real_esp_tls_server_session_create(esp_tls_cfg_server_t *cfg,int socket,esp_tls_t *tls) {
    ++sdk_calls;
    assert(cfg==expected_cfg&&tls==expected_tls&&socket==expected_socket);
    assert(cfg->marker==37);
    if(socket>=0)assert(nodelay(socket)!=0); // Before any SDK TLS work.
    else assert(warnings.size()==1); // The failed option is reported once.
    ++tls->marker;
    return sdk_result;
}
'''

SUITE = r'''
int main() {
    httpd_handle_t handle=nullptr;
    assert(web::start(handle,"amity-test","fixture-authentication")==ESP_OK);
    assert(handle&&credentials_calls==1&&identity_calls==1&&start_calls==1);
    assert(!captured.httpd.open_fn&&captured.httpd.uri_match_fn==httpd_uri_match_wildcard);
    assert(captured.httpd.max_uri_handlers==32&&captured.httpd.stack_size==12288);
    assert(captured.httpd.max_open_sockets==8&&captured.httpd.lru_purge_enable);
    assert(captured.httpd.recv_wait_timeout==15&&captured.httpd.send_wait_timeout==15);
    assert(captured.secure_transport&&!captured.session_tickets);
    assert(captured.servercert==reinterpret_cast<const uint8_t*>(web::certificate.c_str()));
    assert(captured.servercert_len==web::certificate.size()+1);
    assert(captured.prvtkey_pem==reinterpret_cast<const uint8_t*>(web::private_key.c_str()));
    assert(captured.prvtkey_len==web::private_key.size()+1);
    // Each accepted descriptor is configured before the real SDK entry is
    // invoked. No late session callback or replacement connection is needed.
    esp_tls_cfg_server_t cfg;esp_tls_t tls;
    expected_cfg=&cfg;expected_tls=&tls;
    Socket first,second;
    assert(nodelay(first.fd)==0&&nodelay(second.fd)==0);
    expected_socket=first.fd;
    assert(__wrap_esp_tls_server_session_create(&cfg,first.fd,&tls)==0);
    assert(nodelay(first.fd)!=0&&nodelay(second.fd)==0&&warnings.empty()&&sdk_calls==1&&tls.marker==42);
    expected_socket=second.fd;sdk_result=-0x7780;
    assert(__wrap_esp_tls_server_session_create(&cfg,second.fd,&tls)==-0x7780);
    assert(nodelay(second.fd)!=0&&warnings.empty()&&sdk_calls==2&&tls.marker==43&&start_calls==1);
    // Report an option failure without retrying or replacing SDK arguments,
    // then propagate even an arbitrary SDK failure unchanged.
    expected_socket=-1;sdk_result=-1234;
    assert(__wrap_esp_tls_server_session_create(&cfg,-1,&tls)==-1234);
    assert(warnings.size()==1&&sdk_calls==3&&tls.marker==44);
    assert(warnings.front().find("TCP_NODELAY")!=std::string::npos);
    assert(warnings.front().find(std::to_string(EBADF))!=std::string::npos);
    assert(nodelay(first.fd)!=0&&nodelay(second.fd)!=0&&start_calls==1);
    // Neither startup failure nor option failure opens a replacement server.
    handle=nullptr;credentials_result=37;
    assert(web::start(handle,"amity-test","fixture-authentication")==37);
    assert(!handle&&credentials_calls==2&&identity_calls==1&&start_calls==1);
    credentials_result=ESP_OK;identity_result=41;
    assert(web::start(handle,"amity-test","fixture-authentication")==41);
    assert(!handle&&credentials_calls==3&&identity_calls==2&&start_calls==1);
    identity_result=ESP_OK;start_result=43;
    assert(web::start(handle,"amity-test","fixture-authentication")==43);
    assert(!handle&&start_calls==2&&warnings.size()==1);
    std::cout<<"PASS actual HTTPS startup/pre-handshake policy: native TCP_NODELAY before SDK entry; exact cfg/fd/tls and results, one warning on option failure; credentials and connection capacity preserved\n";
}
'''


def main():
    source = (ROOT / 'components/web/web.cpp').read_text()
    actual = ''.join(function(source, name) for name in
                     ('void configure_tcp_socket(', 'esp_err_t start(httpd_handle_t &handle,'))
    wrapper = function(source, 'extern "C" int __wrap_esp_tls_server_session_create(')
    assert '"-Wl,--wrap=esp_tls_server_session_create"' in (ROOT / 'components/web/CMakeLists.txt').read_text()
    with tempfile.TemporaryDirectory(prefix='amity-web-session-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(PREFIX + actual + SDK + wrapper + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
