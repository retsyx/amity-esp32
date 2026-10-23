/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 */
#include "web.hpp"
#include "system_services.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

using Clock=std::chrono::steady_clock;
struct esp_timer {
    esp_timer_create_args_t args;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread thread;
    bool stopped=false;
};
unsigned fake_reboots=0;
static unsigned live_timers=0,ota_begins=0,ota_aborts=0,ota_ends=0,ota_boots=0;
static uint64_t last_timeout=0,shorten_timeout=0;
static bool fail_create=false,fail_start=false;
static std::vector<uint8_t> written;
static esp_app_desc_t running{ESP_APP_DESC_MAGIC_WORD,"1.2.3","amity"};
static esp_partition_t partition{1024*1024};
int64_t esp_timer_get_time() {return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *timer) {
    if(fail_create)return ESP_FAIL;
    *timer=new esp_timer;(*timer)->args=*args;++live_timers;return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t timeout) {
    last_timeout=timeout;if(fail_start)return ESP_FAIL;
    const auto duration=std::chrono::microseconds(shorten_timeout?shorten_timeout:timeout);
    timer->thread=std::thread([timer,duration] {
        std::unique_lock<std::mutex> lock(timer->mutex);
        if(timer->changed.wait_for(lock,duration,[timer]{return timer->stopped;}))return;
        lock.unlock();timer->args.callback(timer->args.arg);
    });return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer,uint32_t) {
    {std::lock_guard<std::mutex> lock(timer->mutex);timer->stopped=true;}
    timer->changed.notify_one();if(timer->thread.joinable())timer->thread.join();return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer) {assert(!timer->thread.joinable());delete timer;--live_timers;return ESP_OK;}
int httpd_req_to_sockfd(httpd_req_t *req) {return req->socket;}
int httpd_req_recv(httpd_req_t *req,char *buffer,size_t size) {
    ++req->receive_calls;
    if(req->socket_input) {
        size_t got=0;
        do {
            const auto n=recv(req->socket,buffer+got,size-got,0);
            if(n<=0)return static_cast<int>(n);
            got+=n;req->received+=n;
        } while(req->wait_record&&got<size);
        return static_cast<int>(got);
    }
    const size_t n=std::min({size,req->chunk,req->input.size()-req->offset});
    memcpy(buffer,req->input.data()+req->offset,n);req->offset+=n;return static_cast<int>(n);
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,const char *key,char *out,size_t size) {
    auto it=req->request_headers.find(key);
    if(it==req->request_headers.end()||it->second.size()>=size)return ESP_FAIL;
    strcpy(out,it->second.c_str());return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *value) {req->status=value;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *) {return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *key,const char *value) {req->headers[key]=value;return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t *req,const char *value,size_t size) {req->response.assign(value,size);return ESP_OK;}
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *value) {return httpd_resp_send(req,value,strlen(value));}
const esp_app_desc_t *esp_app_get_description() {return &running;}
const esp_partition_t *esp_ota_get_running_partition() {return &partition;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) {return &partition;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *,esp_ota_img_states_t *state) {*state=ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {return ESP_OK;}
esp_err_t esp_ota_begin(const esp_partition_t *,size_t,esp_ota_handle_t *handle) {++ota_begins;written.clear();*handle=1;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t,const void *data,size_t size) {const auto *p=static_cast<const uint8_t *>(data);written.insert(written.end(),p,p+size);return ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t) {++ota_aborts;return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t) {++ota_ends;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *) {++ota_boots;return ESP_OK;}

static httpd_req_t request(std::string input,size_t extra=0) {
    httpd_req_t req;req.input=std::move(input);req.content_len=req.input.size()+extra;return req;
}
static std::string firmware(const char *version="1.2.4") {
    esp_image_header_t image{};image.magic=ESP_IMAGE_HEADER_MAGIC;image.chip_id=CONFIG_IDF_FIRMWARE_CHIP_ID;
    esp_image_segment_header_t segment{};
    esp_app_desc_t app=running;strcpy(app.version,version);
    std::string bytes(reinterpret_cast<char *>(&image),sizeof(image));
    bytes.append(reinterpret_cast<char *>(&segment),sizeof(segment));
    bytes.append(reinterpret_cast<char *>(&app),sizeof(app));bytes.append(4096,'x');return bytes;
}
struct SocketPair {
    int fd[2];
    SocketPair() {assert(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);}
    ~SocketPair() {close(fd[0]);close(fd[1]);}
};
static void rejection_tests() {
    auto req=request("",1024*1024);
    assert(web::error(&req,"Login required","401 Unauthorized")==ESP_FAIL);
    assert(req.status=="401 Unauthorized"&&req.headers["Connection"]=="close");
    assert(req.response=="{\"error\":\"Login required\"}"&&req.receive_calls==0);
    req=request("",513);std::string body;
    assert(!web::body(&req,body,512));
    assert(req.status=="413 Content Too Large"&&req.headers["Connection"]=="close"&&req.receive_calls==0);
    for(bool start_failure:{false,true}) {
        fail_create=!start_failure;fail_start=start_failure;req=request("abc");
        assert(!web::body(&req,body));assert(req.status=="503 Service Unavailable"&&req.receive_calls==0);
        assert(live_timers==0);
    }
    fail_create=fail_start=false;
}
static void regular_body_tests() {
    auto req=request("{\"password\":\"secret\"}");req.chunk=3;std::string body;
    assert(web::body(&req,body,512)&&body==req.input&&req.offset==req.content_len);
    assert(last_timeout==15000000&&live_timers==0);
    assert(web::json(&req,"{\"ok\":true}")==ESP_OK&&req.headers.count("Connection")==0);
    req=request("");assert(web::body(&req,body)&&body.empty()&&live_timers==0);
    req=request("partial",10);assert(!web::body(&req,body));
    assert(req.status=="408 Request Timeout"&&req.headers["Connection"]=="close"&&live_timers==0);
}
static void slow_record_test() {
    SocketPair pair;auto req=request("",40);req.socket=pair.fd[0];req.socket_input=req.wait_record=true;
    std::thread sender([&]{for(unsigned i=0;i<40;++i) {if(send(pair.fd[1],"x",1,0)<=0)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}});
    const auto started=Clock::now();
    {
        web::BodyReader reader(&req,100000);assert(reader.valid());char buffer[40];
        assert(reader.receive(buffer,sizeof(buffer))<=0);
    }
    assert(Clock::now()-started<std::chrono::milliseconds(500));
    assert(req.receive_calls==1&&req.received>1&&req.received<40&&live_timers==0);
    sender.join();
}
static void slow_body_test() {
    SocketPair pair;auto req=request("",512);req.socket=pair.fd[0];req.socket_input=true;
    std::thread sender([&]{for(unsigned i=0;i<512;++i) {if(send(pair.fd[1],"x",1,0)<=0)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}});
    shorten_timeout=100000;std::string body;const auto started=Clock::now();
    assert(!web::body(&req,body,512));shorten_timeout=0;
    assert(Clock::now()-started<std::chrono::milliseconds(500));
    assert(req.status=="408 Request Timeout"&&req.headers["Connection"]=="close");
    assert(last_timeout==15000000&&req.received<512&&live_timers==0);sender.join();
}
static void firmware_tests() {
    auto req=request(firmware());req.chunk=31;
    assert(system_services::upload_firmware(&req)==ESP_OK);
    assert(written.size()==req.content_len&&memcmp(written.data(),req.input.data(),written.size())==0);
    assert(ota_begins==1&&ota_ends==1&&ota_boots==1&&fake_reboots==1&&ota_aborts==0);
    assert(last_timeout==120000000&&req.headers.count("Connection")==0&&live_timers==0);
    req=request("",partition.size+1);assert(system_services::upload_firmware(&req)==ESP_FAIL);
    assert(req.receive_calls==0&&req.headers["Connection"]=="close"&&ota_begins==1);
    req=request(firmware("1.2.2"));assert(system_services::upload_firmware(&req)==ESP_FAIL);
    assert(req.status=="409 Conflict"&&req.headers["Connection"]=="close"&&req.offset<req.content_len&&ota_begins==1);
    req=request(firmware("1.2.2"));req.request_headers["X-Amity-Confirm-Version"]="1.2.3";
    assert(system_services::upload_firmware(&req)==ESP_OK&&ota_begins==2&&ota_boots==2);
    req=request(firmware(),5);assert(system_services::upload_firmware(&req)==ESP_FAIL);
    assert(ota_begins==3&&ota_aborts==1&&ota_boots==2&&req.headers["Connection"]=="close");
    // Timeout after an accepted header must abort the inactive OTA slot without selecting it.
    SocketPair pair;const auto image=firmware();
    constexpr size_t header=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    assert(send(pair.fd[1],image.data(),header,0)==static_cast<int>(header));
    req=request("",image.size());req.socket=pair.fd[0];req.socket_input=true;
    shorten_timeout=100000;const auto started=Clock::now();
    assert(system_services::upload_firmware(&req)==ESP_FAIL);shorten_timeout=0;
    assert(Clock::now()-started<std::chrono::milliseconds(500));
    assert(ota_begins==4&&ota_aborts==2&&ota_boots==2&&req.headers["Connection"]=="close"&&live_timers==0);
}
int main() {
    signal(SIGPIPE,SIG_IGN);
    rejection_tests();regular_body_tests();slow_record_test();slow_body_test();firmware_tests();
    puts("HTTPS body deadlines, TLS-fragment interruption, close-on-rejection and OTA safety passed");
}
