// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "system_services.hpp"
#include "amity/consortium/runtime.hpp"
#include "device_settings.hpp"
#include "web.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <sys/socket.h>

namespace system_services {
namespace {
namespace c=amity::consortium;
struct Download {httpd_req_t* request=nullptr;amity::system::OctopusId local;};
std::mutex download_mutex;
std::condition_variable download_finished;
std::atomic<bool> stopping{false};
QueueHandle_t download_queue=nullptr;
TaskHandle_t download_task=nullptr;
httpd_req_t* active_request=nullptr;
int active_socket=-1;
std::string section(std::string_view role,std::string_view hostname,amity::system::OctopusId id,std::string_view boot){
    std::string result="\n===== ";result+=role;
    if(!hostname.empty()){result+=": ";result+=hostname;}
    result+=" =====\nDevice: ";result+=id.str();result+="\nBoot: ";result+=boot.empty()?"unknown":boot;result+="\n\n";
    return result;
}
const char* unavailable(esp_err_t error,bool partial){
    if(partial)return "\n[Log collection interrupted; some logs may be missing.]\n";
    if(error==ESP_ERR_TIMEOUT)return "[Logs unavailable: the device did not respond in time.]\n";
    if(error==ESP_ERR_NO_MEM)return "[Logs unavailable: insufficient memory or the device is busy.]\n";
    return "[Logs unavailable: the device could not be reached or is not ready.]\n";
}
esp_err_t stream_logs(httpd_req_t* req,amity::system::OctopusId local){
    if(stopping.load())return ESP_FAIL;
    httpd_resp_set_type(req,"text/plain; charset=utf-8");
    httpd_resp_set_hdr(req,"Content-Disposition","attachment; filename=amity.log");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    bool connected=true;
    const auto stream=[&](std::string_view bytes){
        if(stopping.load())return connected=false;
        if(!bytes.empty()&&httpd_resp_send_chunk(req,bytes.data(),bytes.size())!=ESP_OK)connected=false;
        return connected;
    };
    const bool main=c::runtime_status().coordinator;
    if(!stream(section(main?"Main Amity":"Amity",device_settings::hostname(),local,boot_id())))return ESP_FAIL;
    {
        // Free the local snapshot before fetching any secondary. The main
        // retains only the current transport chunk while members are read.
        const auto snapshot=log_snapshot();
        if(!snapshot){if(!stream("[Local logs unavailable: insufficient memory.]\n"))return ESP_FAIL;}
        else if(!snapshot.size){if(!stream("[No logs captured.]\n"))return ESP_FAIL;}
        else for(size_t offset=0;offset<snapshot.size;offset+=c::log_chunk_bytes)
            if(!stream({snapshot.bytes.get()+offset,std::min(c::log_chunk_bytes,snapshot.size-offset)}))return ESP_FAIL;
    }
    if(main)for(const auto& peer:c::runtime_peers()){
        if(!peer.enrolled||peer.provisional||peer.id==local)continue;
        const auto peer_boot=peer.boot_id?peer.boot_id.str():std::string{};
        if(!stream(section("Amity",peer.hostname,peer.id,peer_boot)))return ESP_FAIL;
        size_t received=0;
        const auto result=c::runtime_pull_logs(peer.id,[&](std::string_view bytes){
            if(!stream(bytes))return false;
            received+=bytes.size();return true;
        });
        if(!connected)return ESP_FAIL;
        if(result!=ESP_OK){if(!stream(unavailable(result,received!=0)))return ESP_FAIL;}
        else if(!received&&!stream("[No logs captured.]\n"))return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req,nullptr,0);
}
void download_worker(void*){
    for(;;){
        Download job;
        if(xQueueReceive(download_queue,&job,portMAX_DELAY)!=pdTRUE)continue;
        if(!job.request)break;
        const auto result=stream_logs(job.request,job.local);
        {
            std::lock_guard<std::mutex> guard(download_mutex);
            // Keep the session owned until both transmission and cancellation
            // finish. A failed chunked response cannot reuse its connection.
            if(result!=ESP_OK&&active_socket>=0)shutdown(active_socket,SHUT_RDWR);
            active_socket=-1;
        }
        // Completion can wait for HTTPD's control mailbox. Do not hold a mutex
        // that an HTTP handler needs while that task drains its mailbox.
        const auto completed=httpd_req_async_handler_complete(job.request);
        if(completed!=ESP_OK)ESP_LOGW("logs","Could not wake HTTP server after log download: %d",int(completed));
        {
            std::lock_guard<std::mutex> guard(download_mutex);
            active_request=nullptr;
        }
        download_finished.notify_all();
    }
    {
        std::lock_guard<std::mutex> guard(download_mutex);
        download_task=nullptr;
    }
    download_finished.notify_all();
    vTaskDeleteWithCaps(nullptr);
}
}
esp_err_t download_logs(httpd_req_t* req,amity::system::OctopusId local){
    std::unique_lock<std::mutex> guard(download_mutex);
    if(stopping.load())return web::error(req,"Administration is stopping.","503 Service Unavailable");
    if(active_request)return web::error(req,"A log download is already in progress.","503 Service Unavailable");
    // One lazy worker and one admitted download, regardless of browser count.
    // The worker retains only the local snapshot or one member chunk at a time.
    if(!download_task){
        download_queue=xQueueCreate(1,sizeof(Download));
        if(!download_queue)return web::error(req,"Cannot start log download.","503 Service Unavailable");
        if(xTaskCreateWithCaps(download_worker,"log_download",8192,nullptr,2,&download_task,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){
            vQueueDelete(download_queue);download_queue=nullptr;
            return web::error(req,"Cannot start log download.","503 Service Unavailable");
        }
    }
    httpd_req_t* async=nullptr;
    if(httpd_req_async_handler_begin(req,&async)!=ESP_OK)
        return web::error(req,"Cannot start log download.","503 Service Unavailable");
    active_request=async;
    active_socket=httpd_req_to_sockfd(async);
    const Download job{async,local};
    if(xQueueSend(download_queue,&job,0)!=pdTRUE){
        active_socket=-1;guard.unlock();
        httpd_req_async_handler_complete(async);
        guard.lock();active_request=nullptr;guard.unlock();download_finished.notify_all();
        return web::error(req,"Cannot start log download.","503 Service Unavailable");
    }
    return ESP_OK;
}
void stop_log_downloads(){
    std::unique_lock<std::mutex> guard(download_mutex);
    stopping.store(true);
    // shutdown interrupts a blocked TLS write without closing/reusing its fd.
    // The peer collector already bounds each pending chunk wait to two seconds.
    if(active_socket>=0)shutdown(active_socket,SHUT_RDWR);
    download_finished.wait(guard,[]{return !active_request;});
    if(download_task){
        const Download stop{};
        xQueueSend(download_queue,&stop,portMAX_DELAY);
        download_finished.wait(guard,[]{return !download_task;});
        vQueueDelete(download_queue);download_queue=nullptr;
    }
}
}
