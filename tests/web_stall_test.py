#!/usr/bin/env python3
"""Run the actual HTTPS stall deadlines against HTTPD seams and native sockets.

The pinned SDK must keep calling each wrapped function from another source
file, or the linker would silently bypass the deadline. Only the HTTPS server
may create esp-tls server sessions, since its handshake deadline is global.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))

FAKES = {
    'esp_http_server.h': r'''
#pragma once
#include <cstddef>
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0;
using httpd_handle_t=void*;
struct httpd_req {int fd;};
using httpd_req_t=httpd_req;
inline int httpd_req_to_sockfd(httpd_req_t *request){return request->fd;}
''',
    'esp_httpd_priv.h': r'''
#pragma once
#include "esp_http_server.h"
struct sock_db {int fd;};
struct httpd_req_aux {size_t remaining_len;};
struct httpd_data {httpd_req hd_req;httpd_req_aux hd_req_aux;};
''',
    'esp_timer.h': r'''
#pragma once
#include <cstdint>
using esp_err_t=int;
struct esp_timer {void (*callback)(void*);bool armed;uint64_t timeout;unsigned stops;};
using esp_timer_handle_t=esp_timer*;
struct esp_timer_create_args_t {void (*callback)(void*)=nullptr;void *arg=nullptr;int dispatch_method=0;const char *name=nullptr;};
inline esp_timer timer_state{};
inline unsigned timer_creates=0;
inline int create_result=0;
inline esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){
    ++timer_creates;if(create_result)return create_result;
    timer_state.callback=args->callback;*out=&timer_state;return 0;
}
inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t timeout){
    if(timer->armed)return 0x103;timer->armed=true;timer->timeout=timeout;return 0;
}
inline esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer,uint32_t){timer->armed=false;++timer->stops;return 0;}
// The esp_timer task runs an armed timer's callback at expiry.
inline void expire_now(){if(timer_state.armed){timer_state.armed=false;timer_state.callback(nullptr);}}
''',
    'freertos/FreeRTOS.h': '#pragma once\n#define portMAX_DELAY 0xffffffffu\n',
}

SUITE = r'''
#include "stall.hpp"
#include "esp_httpd_priv.h"
#include "esp_timer.h"
#include <cassert>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
extern "C" {
esp_err_t __wrap_httpd_req_new(httpd_data*,sock_db*);
esp_err_t __wrap_httpd_uri(httpd_data*);
esp_err_t __wrap_httpd_req_delete(httpd_data*);
}
struct Pair {
    int fds[2];
    Pair(){assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));}
    ~Pair(){close(fds[0]);close(fds[1]);}
    // The server's blocked receive ends once its socket is shut down.
    bool ended(){char byte;return recv(fds[0],&byte,1,MSG_DONTWAIT)==0;}
};
httpd_data secure_server{},other_server{};
httpd_data *guarded=&secure_server; // the server the fakes expect deadlines for
std::string events;
bool expire_head=false,expire_purge=false;
int head_socket=-1;
// SDK seams: parse the head, route it, then run the handler.
extern "C" esp_err_t __real_httpd_uri(httpd_data *server){
    // Handlers run without the head deadline.
    assert(!timer_state.armed);events+="handler,";(void)server;return ESP_OK;
}
extern "C" esp_err_t __real_httpd_req_new(httpd_data *server,sock_db *session){
    if(server==guarded)
        assert(timer_state.armed&&timer_state.timeout==web::stall::head_us&&session->fd==head_socket);
    else assert(!timer_state.armed);
    events+="head,";
    if(expire_head){expire_now();return -1;}
    return __wrap_httpd_uri(server);
}
extern "C" esp_err_t __real_httpd_req_delete(httpd_data *server){
    const bool purge=server==guarded&&server->hd_req_aux.remaining_len;
    assert(timer_state.armed==purge&&(!purge||timer_state.timeout==web::stall::purge_us));
    events+=purge?"purge,":"delete,";
    if(expire_purge){expire_now();return -1;}
    return ESP_OK;
}
int main(){
    // Phases before the timer exists are unguarded rather than failing.
    web::stall::arm(3,1);web::stall::disarm();assert(timer_creates==0);
    create_result=0x101;assert(web::stall::prepare()==0x101);
    create_result=0;assert(web::stall::prepare()==ESP_OK&&web::stall::prepare()==ESP_OK&&timer_creates==2);
    web::stall::serve(&secure_server);
    {
        // A complete head is routed before the handler, and nothing stays armed.
        Pair pair;head_socket=pair.fds[0];sock_db session{pair.fds[0]};
        assert(__wrap_httpd_req_new(&secure_server,&session)==ESP_OK&&events=="head,handler,");
        assert(!timer_state.armed&&!pair.ended());
        // A head still incomplete at its deadline ends the blocked receive.
        events.clear();expire_head=true;
        assert(__wrap_httpd_req_new(&secure_server,&session)==-1&&events=="head,"&&pair.ended()&&!timer_state.armed);
        expire_head=false;
    }
    {
        // Only an unread body is purged under a deadline.
        Pair pair;secure_server.hd_req.fd=pair.fds[0];events.clear();
        secure_server.hd_req_aux.remaining_len=0;assert(__wrap_httpd_req_delete(&secure_server)==ESP_OK);
        secure_server.hd_req_aux.remaining_len=4096;assert(__wrap_httpd_req_delete(&secure_server)==ESP_OK);
        assert(events=="delete,purge,"&&!timer_state.armed&&!pair.ended());
        expire_purge=true;assert(__wrap_httpd_req_delete(&secure_server)==-1&&pair.ended()&&!timer_state.armed);
        expire_purge=false;secure_server.hd_req_aux.remaining_len=0;
    }
    {
        // Another HTTPD server, such as HomeKit's, passes through unchanged.
        Pair pair;sock_db session{pair.fds[0]};other_server.hd_req.fd=pair.fds[0];
        other_server.hd_req_aux.remaining_len=4096;events.clear();
        assert(__wrap_httpd_req_new(&other_server,&session)==ESP_OK&&__wrap_httpd_req_delete(&other_server)==ESP_OK);
        assert(events=="head,handler,delete,"&&!pair.ended());
        // Releasing another server keeps the HTTPS server guarded; releasing
        // the HTTPS server stops guarding it.
        web::stall::release(&other_server);
        head_socket=pair.fds[0];events.clear();
        assert(__wrap_httpd_req_new(&secure_server,&session)==ESP_OK&&events=="head,handler,");
        web::stall::release(&secure_server);guarded=nullptr;
        secure_server.hd_req.fd=pair.fds[0];secure_server.hd_req_aux.remaining_len=4096;events.clear();
        assert(__wrap_httpd_req_new(&secure_server,&session)==ESP_OK&&__wrap_httpd_req_delete(&secure_server)==ESP_OK);
        assert(events=="head,handler,delete,"&&!pair.ended());
    }
    // The handshake deadline uses the same timer for the session's socket.
    {
        Pair pair;web::stall::arm(pair.fds[0],web::stall::handshake_us);
        assert(timer_state.armed&&timer_state.timeout==web::stall::handshake_us);
        expire_now();assert(pair.ended());web::stall::disarm();
        // Expiry after a disarm cannot reach a reused descriptor.
        Pair reused;expire_now();assert(!reused.ended());
    }
    std::cout<<"PASS actual HTTPS stall deadlines: handshake, request head and unread-body purge bounded on the HTTPS server only; handlers unbounded by them\n";
}
'''


def function_body(source, name):
    start = re.search(r'^[\w ]*\b' + re.escape(name) + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def check_sdk():
    http = IDF / 'components/esp_http_server/src'
    private = (http / 'esp_httpd_priv.h').read_text()
    for declaration in ('esp_err_t httpd_req_new(struct httpd_data *hd, struct sock_db *sd);',
                        'esp_err_t httpd_uri(struct httpd_data *hd);',
                        'esp_err_t httpd_req_delete(struct httpd_data *hd);'):
        assert declaration in private, declaration
    assert re.search(r'struct sock_db \{\s*int fd;', private)
    assert re.search(r'struct httpd_req_aux \{[^}]*size_t\s+remaining_len;', private)
    assert re.search(r'struct httpd_data \{[^}]*struct httpd_req hd_req;[^}]*struct httpd_req_aux hd_req_aux;', private)
    sources = {path.name: path.read_text() for path in http.glob('*.c')}
    definitions = {name: [file for file, text in sources.items()
                          if re.search(r'^esp_err_t ' + name + r'\(struct httpd_data', text, re.M)]
                   for name in ('httpd_req_new', 'httpd_uri', 'httpd_req_delete')}
    assert definitions == {'httpd_req_new': ['httpd_parse.c'], 'httpd_uri': ['httpd_uri.c'],
                           'httpd_req_delete': ['httpd_parse.c']}, definitions
    # Each wrapped call crosses a source file.
    process = function_body(sources['httpd_sess.c'], 'httpd_sess_process')
    assert 'httpd_req_new(hd, session)' in process and 'httpd_req_delete(hd)' in process
    assert 'return httpd_uri(hd);' in function_body(sources['httpd_parse.c'], 'httpd_parse_req')
    https = (IDF / 'components/esp_https_server/src/https_server.c').read_text()
    assert 'esp_tls_server_session_create(' in function_body(https, 'httpd_ssl_open')


def check_handshake_owner():
    callers = []
    for directory in ('components', 'main', 'third_party'):
        for path in (ROOT / directory).rglob('*'):
            if path.suffix in ('.c', '.cpp') and 'esp_tls_server_session_create(' in path.read_text(errors='ignore'):
                callers.append(path.relative_to(ROOT).as_posix())
    assert callers == ['components/web/web.cpp'], callers


def main():
    check_sdk()
    check_handshake_owner()
    cmake = (ROOT / 'components/web/CMakeLists.txt').read_text()
    for name in ('httpd_req_new', 'httpd_uri', 'httpd_req_delete'):
        assert f'"-Wl,--wrap={name}"' in cmake, name
    with tempfile.TemporaryDirectory(prefix='amity-web-stall-') as directory:
        directory = Path(directory)
        for name, text in FAKES.items():
            (directory / name).parent.mkdir(parents=True, exist_ok=True)
            (directory / name).write_text(text)
        cpp, binary = directory / 'test.cpp', directory / 'test'
        cpp.write_text(SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I', str(directory), '-I', str(ROOT / 'components/web'),
            str(ROOT / 'components/web/stall.cpp'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
