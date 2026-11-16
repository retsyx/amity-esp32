/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web.hpp"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
namespace web {
namespace {
// Two connections admit a browser's parallel connection. When both are taken,
// a new connection replaces an answered one, else the oldest: idle or slow
// clients cannot hold the listener. Sockets: the listener plus these clients.
constexpr size_t client_slots=2;
// Request line and headers. The response repeats the target and still fits
// one empty TCP send buffer.
constexpr size_t head_limit=2048;
constexpr int64_t head_us=3000000;   // the whole request head, from accept
constexpr int64_t linger_us=1000000; // discarding an unread body after replying
constexpr int64_t poll_us=250000;    // bounds stop latency
constexpr auto stop_wait=std::chrono::seconds(2);

struct Client {int fd=-1;int64_t accepted=0,deadline=0;bool answered=false;std::string head;};
std::mutex lifecycle;
std::condition_variable finished;
std::atomic<bool> stopping{false};
bool running=false;

bool visible(std::string_view text) {
    return !text.empty()&&std::all_of(text.begin(),text.end(),[](unsigned char c){return c>0x20&&c<0x7f;});
}
bool same(std::string_view a,std::string_view b) {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),
        [](unsigned char x,unsigned char y){return std::tolower(x)==std::tolower(y);});
}
std::string_view trim(std::string_view text) {
    while(!text.empty()&&(text.front()==' '||text.front()=='\t'))text.remove_prefix(1);
    while(!text.empty()&&(text.back()==' '||text.back()=='\t'))text.remove_suffix(1);
    return text;
}
// One past the blank line that ends the head, or npos while it is incomplete.
size_t head_end(std::string_view data) {
    for(auto i=data.find('\n');i!=std::string_view::npos;i=data.find('\n',i+1)) {
        if(i+1<data.size()&&data[i+1]=='\n')return i+2;
        if(i+2<data.size()&&data[i+1]=='\r'&&data[i+2]=='\n')return i+3;
    }
    return std::string_view::npos;
}
void drop(Client &client) {
    if(client.fd>=0)close(client.fd);
    client=Client{};
}
void reply(Client &client,int64_t now) {
    const auto location=redirect_location(client.head);
    const std::string response=location.empty()?
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n":
        "HTTP/1.1 308 Permanent Redirect\r\nLocation: "+location+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    // A short write cannot be completed later; it ends the connection.
    if(send(client.fd,response.data(),response.size(),0)!=static_cast<ssize_t>(response.size())||
       shutdown(client.fd,SHUT_WR)!=0) {drop(client);return;}
    // Read and discard until the client closes, so an unread request body
    // does not reset the connection before the client reads the response.
    client.answered=true;client.deadline=now+linger_us;std::string().swap(client.head);
}
void receive(Client &client,int64_t now) {
    char buffer[512];
    const auto count=recv(client.fd,buffer,sizeof(buffer),0);
    if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return;
    if(count<=0) {drop(client);return;}
    if(client.answered)return;
    client.head.append(buffer,count);
    const auto end=head_end(client.head);
    if(end!=std::string::npos)client.head.resize(end);
    // An oversized head is answered as malformed.
    if(end!=std::string::npos||client.head.size()>=head_limit)reply(client,now);
}
void admit(int listener,std::array<Client,client_slots> &clients,int64_t now) {
    auto slot=std::find_if(clients.begin(),clients.end(),[](const Client &c){return c.fd<0;});
    if(slot==clients.end()) {
        // Replace before accepting, so the socket count never exceeds the slots.
        slot=std::min_element(clients.begin(),clients.end(),[](const Client &a,const Client &b){
            return a.answered!=b.answered?a.answered:a.accepted<b.accepted;});
        drop(*slot);
    }
    const int fd=accept(listener,nullptr,nullptr);
    if(fd<0)return;
    const int flags=fcntl(fd,F_GETFL,0);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)!=0) {close(fd);return;}
    slot->fd=fd;slot->accepted=now;slot->deadline=now+head_us;
}
void serve(void *argument) {
    const int listener=static_cast<int>(reinterpret_cast<intptr_t>(argument));
    std::array<Client,client_slots> clients;
    while(!stopping.load()) {
        fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);
        int top=listener;auto now=esp_timer_get_time();auto wake=now+poll_us;
        for(const auto &client:clients)if(client.fd>=0) {
            FD_SET(client.fd,&readable);top=std::max(top,client.fd);wake=std::min(wake,client.deadline);
        }
        const auto wait=std::max<int64_t>(0,wake-now);
        timeval timeout{static_cast<decltype(timeval::tv_sec)>(wait/1000000),static_cast<decltype(timeval::tv_usec)>(wait%1000000)};
        const int ready=select(top+1,&readable,nullptr,nullptr,&timeout);
        if(ready<0&&errno!=EINTR) {
            ESP_LOGW("web","HTTP redirect select failed: errno %d",errno);
            usleep(poll_us);continue;
        }
        now=esp_timer_get_time();
        // Serve requests already received before admitting new connections.
        if(ready>0)for(auto &client:clients)if(client.fd>=0&&FD_ISSET(client.fd,&readable))receive(client,now);
        if(ready>0&&FD_ISSET(listener,&readable))admit(listener,clients,now);
        for(auto &client:clients)if(client.fd>=0&&now>=client.deadline)drop(client);
    }
    for(auto &client:clients)drop(client);
    close(listener);
    {
        std::lock_guard<std::mutex> guard(lifecycle);
        running=false;
    }
    finished.notify_all();
    vTaskDeleteWithCaps(nullptr);
}
}
std::string redirect_location(std::string_view head) {
    std::string_view target,host;
    bool request_line=true,hosts=false,complete=false;
    while(!complete) {
        const auto end=head.find('\n');
        if(end==std::string_view::npos)return {};
        auto line=head.substr(0,end);head.remove_prefix(end+1);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(request_line) {
            // method SP request-target SP HTTP-version
            const auto first=line.find(' '),last=line.rfind(' ');
            if(first==std::string_view::npos||first==last)return {};
            target=line.substr(first+1,last-first-1);
            if(!visible(line.substr(0,first))||!visible(target)||line.substr(last+1,5)!="HTTP/")return {};
            request_line=false;
        } else if(line.empty())complete=true;
        else {
            const auto colon=line.find(':');
            // Obsolete line folding and nameless fields are malformed.
            if(colon==std::string_view::npos||colon==0||line.front()==' '||line.front()=='\t')return {};
            if(!same(line.substr(0,colon),"Host"))continue;
            if(hosts)return {};
            hosts=true;host=trim(line.substr(colon+1));
        }
    }
    // An absolute-form target is the requested URL itself.
    if(target.size()>7&&same(target.substr(0,7),"http://"))return "https://"+std::string(target.substr(7));
    if(target.front()!='/'||!visible(host)||host.find_first_of("/?#@\\")!=std::string_view::npos)return {};
    return "https://"+std::string(host)+std::string(target);
}
esp_err_t start_redirect(uint16_t port) {
    std::lock_guard<std::mutex> guard(lifecycle);
    if(running)return ESP_ERR_INVALID_STATE;
#if CONFIG_LWIP_IPV6
    // Dual stack, as the HTTPS server.
    const int listener=socket(AF_INET6,SOCK_STREAM,0);
    sockaddr_in6 address{};address.sin6_family=AF_INET6;address.sin6_addr=in6addr_any;address.sin6_port=htons(port);
#else
    const int listener=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_ANY);address.sin_port=htons(port);
#endif
    if(listener<0) {ESP_LOGE("web","HTTP redirect socket failed: errno %d",errno);return ESP_FAIL;}
    const int reuse=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    const int flags=fcntl(listener,F_GETFL,0);
    if(bind(listener,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=0||listen(listener,client_slots)!=0||
       flags<0||fcntl(listener,F_SETFL,flags|O_NONBLOCK)!=0) {
        ESP_LOGE("web","HTTP redirect listener on port %u failed: errno %d",unsigned(port),errno);
        close(listener);return ESP_FAIL;
    }
    stopping.store(false);
    if(xTaskCreateWithCaps(serve,"http_redirect",4096,reinterpret_cast<void*>(static_cast<intptr_t>(listener)),2,nullptr,
                           MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {close(listener);return ESP_ERR_NO_MEM;}
    running=true;
    return ESP_OK;
}
esp_err_t stop_redirect() {
    std::unique_lock<std::mutex> guard(lifecycle);
    stopping.store(true);
    return finished.wait_for(guard,stop_wait,[]{return !running;})?ESP_OK:ESP_ERR_TIMEOUT;
}
}
