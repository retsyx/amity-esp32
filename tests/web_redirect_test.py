#!/usr/bin/env python3
"""Run the actual HTTP redirect listener against native loopback TCP sockets.

Only the task, timer, log and SDK type headers are seams. Slow, idle and
evicted clients, unread bodies, and stop/restart use the host's real sockets.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

FAKES = {
    'esp_http_server.h': r'''
#pragma once
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_STATE=0x103,ESP_ERR_TIMEOUT=0x107;
using httpd_handle_t=void*;
enum httpd_method_t {HTTP_GET=1,HTTP_POST=3};
struct httpd_req_t;
''',
    'esp_timer.h': r'''
#pragma once
#include <chrono>
#include <cstdint>
using esp_timer_handle_t=void*;
inline int64_t esp_timer_get_time(){
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
''',
    'sdkconfig.h': '#pragma once\n#define CONFIG_LWIP_IPV6 1\n',
    'esp_heap_caps.h': '#pragma once\n#define MALLOC_CAP_8BIT 4u\n#define MALLOC_CAP_SPIRAM 1024u\n',
    'esp_log.h': '#pragma once\n#include <cstdio>\n'
                 '#define ESP_LOGE(tag,format,...) std::fprintf(stderr,"E %s: " format "\\n",tag,__VA_ARGS__)\n'
                 '#define ESP_LOGW(tag,format,...) std::fprintf(stderr,"W %s: " format "\\n",tag,__VA_ARGS__)\n',
    'freertos/FreeRTOS.h': '#pragma once\nusing BaseType_t=int;using UBaseType_t=unsigned;using TaskHandle_t=void*;\n'
                           'constexpr BaseType_t pdPASS=1;\n',
    'freertos/idf_additions.h': r'''
#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
using TaskFunction_t=void(*)(void*);
struct TaskRecord {std::string name;uint32_t stack;UBaseType_t priority;uint32_t caps;};
inline std::vector<TaskRecord> task_records;
inline std::vector<std::thread> task_threads;
inline BaseType_t xTaskCreateWithCaps(TaskFunction_t task,const char *name,uint32_t stack,void *argument,
                                      UBaseType_t priority,TaskHandle_t *handle,uint32_t caps){
    task_records.push_back({name,stack,priority,caps});
    if(handle)*handle=nullptr;
    task_threads.emplace_back(task,argument);
    return pdPASS;
}
inline void vTaskDeleteWithCaps(TaskHandle_t){}
''',
}

SUITE = r'''
#include "web.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include <thread>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std::chrono;
using namespace std::string_literals;
const std::string bad="HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
std::string moved(const std::string &location){
    return "HTTP/1.1 308 Permanent Redirect\r\nLocation: "+location+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}
uint16_t free_port(){
    const int fd=socket(AF_INET6,SOCK_STREAM,0);assert(fd>=0);
    sockaddr_in6 address{};address.sin6_family=AF_INET6;address.sin6_addr=in6addr_any;
    assert(!bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)));
    socklen_t size=sizeof(address);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size));
    close(fd);return ntohs(address.sin6_port);
}
uint16_t port;
int connect_to(bool v6=false){
    int fd;
    if(v6){
        fd=socket(AF_INET6,SOCK_STREAM,0);sockaddr_in6 address{};address.sin6_family=AF_INET6;
        address.sin6_addr=in6addr_loopback;address.sin6_port=htons(port);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){close(fd);return -1;}
    }else{
        fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in address{};address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){close(fd);return -1;}
    }
    timeval timeout{6,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    std::this_thread::sleep_for(milliseconds(50)); // accepted in arrival order
    return fd;
}
void put(int fd,const std::string &data){assert(send(fd,data.data(),data.size(),0)==static_cast<ssize_t>(data.size()));}
// Everything the server sends before closing; a reset is not a close.
std::string response(int fd){
    std::string data;char buffer[1024];
    for(;;){const auto n=recv(fd,buffer,sizeof(buffer),0);if(!n)return data;assert(n>0);data.append(buffer,n);}
}
bool pending(int fd){pollfd p{fd,POLLIN,0};return poll(&p,1,0)==0;}
// Milliseconds until the server closed the connection without sending anything.
long closed_after(int fd,steady_clock::time_point since){
    char byte;const auto n=recv(fd,&byte,1,0);
    assert(n==0||(n<0&&errno==ECONNRESET));
    return duration_cast<milliseconds>(steady_clock::now()-since).count();
}
std::string ask(const std::string &request,bool v6=false){
    const int fd=connect_to(v6);assert(fd>=0);put(fd,request);
    auto result=response(fd);close(fd);return result;
}
int main(){
    std::signal(SIGPIPE,SIG_IGN);
    using web::redirect_location;
    // The requested URL is unchanged apart from its scheme.
    assert(redirect_location("GET / HTTP/1.1\r\nHost: amity.local\r\n\r\n")=="https://amity.local/");
    assert(redirect_location("GET /api/status?view=all&x=%20 HTTP/1.1\r\nHost: amity.local\r\n\r\n")==
           "https://amity.local/api/status?view=all&x=%20");
    assert(redirect_location("POST /a/b/ HTTP/1.0\r\nUser-Agent: x\r\nhOsT: \t Living-Room.local:8080 \r\nAccept: */*\r\n\r\n")==
           "https://Living-Room.local:8080/a/b/");
    assert(redirect_location("GET / HTTP/1.1\nHost: [fe80::1]\n\n")=="https://[fe80::1]/");
    // An absolute-form target already names the host.
    assert(redirect_location("GET http://amity.local/x?y=1 HTTP/1.1\r\nHost: other.local\r\n\r\n")=="https://amity.local/x?y=1");
    assert(redirect_location("GET HTTP://Amity.local/ HTTP/1.0\r\n\r\n")=="https://Amity.local/");
    // Malformed heads, missing or repeated hosts, and targets that are not
    // paths cannot be redirected.
    for(const char *head:{"GET / HTTP/1.1\r\n\r\n","GET / HTTP/1.1\r\nHost:\r\n\r\n",
                          "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n","GET / HTTP/1.1\r\nHost: a\r\n",
                          "OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n","GET https://a/ HTTP/1.1\r\nHost: a\r\n\r\n",
                          "GET http:// HTTP/1.1\r\nHost: a\r\n\r\n","GET /\r\nHost: a\r\n\r\n","GET / SIP/2.0\r\nHost: a\r\n\r\n",
                          "GET  / HTTP/1.1\r\nHost: a\r\n\r\n","GET /\x01 HTTP/1.1\r\nHost: a\r\n\r\n",
                          "GET / HTTP/1.1\r\nHost: a b\r\n\r\n","GET / HTTP/1.1\r\nHost: evil/x?\r\n\r\n",
                          "GET / HTTP/1.1\r\nHost: user@a\r\n\r\n","GET / HTTP/1.1\r\nX: 1\r\n folded\r\nHost: a\r\n\r\n",
                          "GET / HTTP/1.1\r\n: x\r\nHost: a\r\n\r\n","\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n",""})
        assert(redirect_location(head).empty());

    port=free_port();
    assert(web::start_redirect(port)==ESP_OK&&web::start_redirect(port)==ESP_ERR_INVALID_STATE);
    assert(task_records.size()==1&&task_records[0].caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    // IPv4 and IPv6 clients share the dual-stack listener.
    assert(ask("GET /a?b=1 HTTP/1.1\r\nHost: amity.local\r\n\r\n")==moved("https://amity.local/a?b=1"));
    assert(ask("HEAD / HTTP/1.1\r\nHost: amity.local\r\n\r\n",true)==moved("https://amity.local/"));
    assert(ask("GET / HTTP/1.1\r\n\r\n")==bad);
    // A head arriving in pieces is answered once complete.
    {
        const int fd=connect_to();put(fd,"GET /x HT");std::this_thread::sleep_for(milliseconds(100));
        put(fd,"TP/1.1\r\nHo");std::this_thread::sleep_for(milliseconds(100));put(fd,"st: a\r\n\r\n");
        assert(response(fd)==moved("https://a/x"));close(fd);
    }
    // An oversized head is rejected without waiting for its end.
    assert(ask("GET /"+std::string(3000,'a'))==bad);
    // An unread request body cannot reset the connection before the client
    // has read the response.
    assert(ask("POST /api/login HTTP/1.1\r\nHost: amity.local\r\nContent-Length: 100000\r\n\r\n"+std::string(20000,'x'))==
           moved("https://amity.local/api/login"));
    // A client stalled mid-head does not delay another client, and is closed
    // without a response when its head deadline expires.
    {
        const auto started=steady_clock::now();
        const int slow=connect_to();put(slow,"GET / HTTP/1.1\r\nHost: a");
        const auto asked=steady_clock::now();
        assert(ask("GET /fast HTTP/1.1\r\nHost: a\r\n\r\n")==moved("https://a/fast"));
        assert(steady_clock::now()-asked<milliseconds(500)&&pending(slow));
        const auto elapsed=closed_after(slow,started);
        assert(elapsed>=2700&&elapsed<4500);close(slow);
    }
    // With both slots held, a new client replaces the oldest unanswered one.
    {
        const int oldest=connect_to(),newer=connect_to();
        assert(ask("GET /c HTTP/1.1\r\nHost: a\r\n\r\n")==moved("https://a/c"));
        assert(closed_after(oldest,steady_clock::now())<500&&pending(newer));
        // An answered client still lingering is replaced before an older unanswered one.
        const int answered=connect_to();put(answered,"GET /d HTTP/1.1\r\nHost: a\r\n\r\n");
        std::this_thread::sleep_for(milliseconds(100));
        const int next=connect_to();
        assert(pending(newer));
        put(next,"GET /e HTTP/1.1\r\nHost: a\r\n\r\n");assert(response(next)==moved("https://a/e"));
        assert(response(answered)==moved("https://a/d"));
        close(oldest);close(newer);close(answered);close(next);
    }
    // Stop releases the port promptly; a later start serves again.
    {
        const auto started=steady_clock::now();
        assert(web::stop_redirect()==ESP_OK&&steady_clock::now()-started<milliseconds(1000));
        assert(web::stop_redirect()==ESP_OK);
        assert(connect_to()<0);
        assert(web::start_redirect(port)==ESP_OK);
        assert(ask("GET /again HTTP/1.1\r\nHost: a\r\n\r\n")==moved("https://a/again"));
        assert(web::stop_redirect()==ESP_OK&&connect_to()<0);
    }
    for(auto &task:task_threads)task.join();
    std::cout<<"PASS actual HTTP redirect listener: 308 to the same URL over HTTPS; slow, idle and lingering clients bounded and replaceable; unread bodies drained; prompt stop\n";
}
'''


def config_value(text, name):
    match = re.search(r'^' + re.escape(name) + r'=(\d+)$', text, re.M)
    assert match, name
    return int(match.group(1))


def check_socket_pool():
    # Each HTTPD holds its listener and two control sockets besides sessions.
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    https = int(re.search(r'cfg\.httpd\.max_open_sockets=(\d+);',
                          (ROOT / 'components/web/web.cpp').read_text()).group(1))
    redirect = 1 + int(re.search(r'constexpr size_t client_slots=(\d+);',
                                 (ROOT / 'components/web/redirect.cpp').read_text()).group(1))
    homekit = config_value(defaults, 'CONFIG_HAP_HTTP_MAX_OPEN_SOCKETS')
    limits = (ROOT / 'components/consortium/include/amity/consortium/limits.hpp').read_text()
    peers = int(re.search(r'maximum_roster_peers=(\d+);', limits).group(1))
    pending = int(re.search(r'maximum_pending_connections=(\d+);', limits).group(1))
    needed = (https + 3) + (homekit + 3) + redirect + (1 + peers + pending)
    available = config_value(defaults, 'CONFIG_LWIP_MAX_SOCKETS')
    assert needed <= available, (needed, available)


def main():
    assert 'esp_err_t start_redirect(uint16_t port=80);' in (ROOT / 'components/web/include/web.hpp').read_text()
    check_socket_pool()
    with tempfile.TemporaryDirectory(prefix='amity-web-redirect-') as directory:
        directory = Path(directory)
        for name, text in FAKES.items():
            (directory / name).parent.mkdir(parents=True, exist_ok=True)
            (directory / name).write_text(text)
        cpp, binary = directory / 'test.cpp', directory / 'test'
        cpp.write_text(SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-pthread',
            '-I', str(directory), '-I', str(ROOT / 'components/web/include'),
            str(ROOT / 'components/web/redirect.cpp'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
