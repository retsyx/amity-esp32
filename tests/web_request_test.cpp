/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 */
#include "web.hpp"
#include "stall.hpp"
#include "system_services.hpp"
#include "core_fakes.hpp"
#include "httpd_fake.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

using Clock=std::chrono::steady_clock;
using amity_fake::HttpRequest;
// web_stall_test.py exercises the guard; here each response records its bound.
static std::vector<size_t> bounded;
web::stall::Response::Response(httpd_req_t*,size_t size) {(void)armed_;bounded.push_back(size);}
web::stall::Response::~Response() {}

static std::unique_ptr<HttpRequest> request(std::string input,size_t extra=0) {
    auto req=std::make_unique<HttpRequest>(HTTP_POST,"/api",std::move(input));
    req->content_len+=extra;req->socket=7;return req;
}
static bool closes(const HttpRequest &req) {return req.response->header("Connection")=="close";}
// The body deadline timer, started at started_us.
static bool body_timer(int64_t started_us,uint64_t timeout_us) {
    const auto timers=amity_fake::timers();
    return !timers.empty()&&timers.back().name=="http_body"&&timers.back().starts==1&&
        timers.back().deadline_us==started_us+int64_t(timeout_us)&&timers.back().deleted;
}
// Once the body timer is armed, waits for transfers to start, then moves the
// scripted clock by advance_us, which fires the timer on this thread.
static std::thread expire_body_timer(int64_t advance_us) {
    return std::thread([advance_us] {
        for(;;) {
            const auto timers=amity_fake::timers();
            if(!timers.empty()&&timers.back().armed)break;
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        amity_fake::advance_us(advance_us);
    });
}
struct SocketPair {
    int fd[2];
    SocketPair() {assert(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);}
    ~SocketPair() {close(fd[0]);close(fd[1]);}
};
static void rejection_tests() {
    {
        auto req=request("",1024*1024);
        assert(web::error(req.get(),"Login required","401 Unauthorized")==ESP_FAIL);
        assert(req->response->status=="401 Unauthorized"&&closes(*req));
        assert(req->response->body=="{\"error\":\"Login required\"}"&&req->receive_calls==0);
    }
    {
        auto req=request("",513);std::string body;
        assert(!web::body(req.get(),body,512));
        assert(req->response->status=="413 Content Too Large"&&closes(*req)&&req->receive_calls==0);
    }
    for(bool start_failure:{false,true}) {
        if(start_failure)amity_fake::fail_timer_starts(1);else amity_fake::fail_timer_creations(1);
        auto req=request("abc");std::string body;
        assert(!web::body(req.get(),body));assert(req->response->status=="503 Service Unavailable"&&req->receive_calls==0);
        assert(amity_fake::live_timers()==0);
    }
}
static void regular_body_tests() {
    {
        auto req=request("{\"password\":\"secret\"}");req->chunk=3;std::string body;
        const auto started=amity_fake::now_us();
        assert(web::body(req.get(),body,512)&&body==req->body&&req->offset==req->content_len);
        assert(body_timer(started,15000000)&&amity_fake::live_timers()==0);
        assert(web::json(req.get(),"{\"ok\":true}")==ESP_OK&&!req->response->has_header("Connection"));
        assert(!bounded.empty()&&bounded.back()==req->response->body.size()&&req->response->body=="{\"ok\":true}");
    }
    {
        auto req=request("");std::string body;
        assert(web::body(req.get(),body)&&body.empty()&&amity_fake::live_timers()==0);
    }
    {
        auto req=request("partial",10);std::string body;
        assert(!web::body(req.get(),body));
        assert(req->response->status=="408 Request Timeout"&&closes(*req)&&amity_fake::live_timers()==0);
    }
}
static void slow_record_test() {
    SocketPair pair;auto req=request("",40);req->socket=pair.fd[0];size_t received=0;
    // A TLS record completes only once all its bytes arrive.
    req->receive=[&](char *buffer,size_t size) {
        size_t got=0;
        do {
            const auto n=recv(pair.fd[0],buffer+got,size-got,0);
            if(n<=0)return static_cast<int>(n);
            got+=n;received+=n;
        } while(got<size);
        return static_cast<int>(got);
    };
    std::thread sender([&]{for(unsigned i=0;i<40;++i) {if(send(pair.fd[1],"x",1,0)<=0)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}});
    std::thread clock=expire_body_timer(100000);
    const auto started=Clock::now();
    {
        web::BodyReader reader(req.get(),100000);assert(reader.valid());char buffer[40];
        assert(reader.receive(buffer,sizeof(buffer))<=0);
    }
    clock.join();
    assert(Clock::now()-started<std::chrono::milliseconds(500));
    assert(req->receive_calls==1&&received>1&&received<40&&amity_fake::live_timers()==0);
    sender.join();
}
static void slow_body_test() {
    SocketPair pair;auto req=request("",512);req->socket=pair.fd[0];size_t received=0;
    req->receive=[&](char *buffer,size_t size) {
        const auto n=recv(pair.fd[0],buffer,size,0);
        if(n>0)received+=n;
        return static_cast<int>(n);
    };
    std::thread sender([&]{for(unsigned i=0;i<512;++i) {if(send(pair.fd[1],"x",1,0)<=0)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}});
    std::thread clock=expire_body_timer(15000000);
    std::string body;const auto started=Clock::now();const auto started_us=amity_fake::now_us();
    assert(!web::body(req.get(),body,512));
    clock.join();
    assert(Clock::now()-started<std::chrono::milliseconds(500));
    assert(req->response->status=="408 Request Timeout"&&closes(*req));
    assert(body_timer(started_us,15000000)&&received<512&&amity_fake::live_timers()==0);sender.join();
}
int main() {
    signal(SIGPIPE,SIG_IGN);
    rejection_tests();regular_body_tests();slow_record_test();slow_body_test();
    puts("HTTPS body deadlines, TLS-fragment interruption, close-on-rejection passed (signed OTA covered by update_bundle_test)");
}
