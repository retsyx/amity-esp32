// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "system_services.hpp"
#include "amity/consortium/runtime.hpp"
#include "device_settings.hpp"
#include "web.hpp"
#include <algorithm>
#include <atomic>
#include <string_view>

namespace system_services {
namespace {
namespace c=amity::consortium;
std::atomic_flag downloading=ATOMIC_FLAG_INIT;
struct DownloadGuard {~DownloadGuard(){downloading.clear(std::memory_order_release);}};
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
}
esp_err_t download_logs(httpd_req_t* req,amity::system::OctopusId local){
    if(downloading.test_and_set(std::memory_order_acquire))
        return web::error(req,"A log download is already in progress.","503 Service Unavailable");
    DownloadGuard guard;
    httpd_resp_set_type(req,"text/plain; charset=utf-8");
    httpd_resp_set_hdr(req,"Content-Disposition","attachment; filename=amity.log");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    bool connected=true;
    const auto stream=[&](std::string_view bytes){
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
}
