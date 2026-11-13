/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web_admin.hpp"
#include "system_admin.hpp"
#include "activity_editor.hpp"
#include "local_remote_approval.hpp"
#include "key_request.hpp"
#include "web.hpp"
#include "config_store.hpp"
#include "device_settings.hpp"
#include "system_services.hpp"
#include "amity_matter.h"
#include "homekit.hpp"
#include "ble_remote.h"
#include "amity/consortium/runtime.hpp"
#include "cJSON.h"
#include "json_input.hpp"
#include "qrcodegen.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <mutex>
#include "cec/bus.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
namespace web_admin {
namespace {
amity::SystemControl *hub;
LocalRemoteApprovalBook remote_approvals;
httpd_handle_t server;
std::mutex server_mutex;
std::atomic_bool ui_enabled{false};
std::atomic_bool ui_revoked{false};
// One line per valid HTTP key request. In particular, keep time spent
// waiting for the requested hold separate from unexpected HTTP latency.
struct KeyRequestTiming {
    explicit KeyRequestTiming(bool enabled):started_us(enabled?esp_timer_get_time():0),enabled(enabled) {}
    void guards_complete() {if(enabled)guarded_us=esp_timer_get_time();}
    void body_complete() {if(enabled)body_us=esp_timer_get_time();}
    void submitting(const KeyRequest& request) {
        if(!enabled)return;
        has_key=true;key=request.key;hold_ms=request.duration_ms;submit_started_us=esp_timer_get_time();
    }
    // Setup-gated or rejected presses never invoke the hold wait callback.
    void holding(uint16_t duration_ms) {hold_wait_ms=duration_ms;}
    void submitted(esp_err_t result) {if(enabled){submit_finished_us=esp_timer_get_time();outcome=result;}}
    void responding() {if(enabled)response_started_us=esp_timer_get_time();}
    esp_err_t finish_response(esp_err_t result) {response_result=result;return result;}
    ~KeyRequestTiming() {
        if(!has_key)return;
        const auto finished_us=esp_timer_get_time(),total_us=finished_us-started_us;
        ESP_LOG_LEVEL(total_us>static_cast<int64_t>(hold_wait_ms)*1000+100000?ESP_LOG_WARN:ESP_LOG_INFO,"web_admin",
            "key=%u hold_ms=%u hold_wait_ms=%u guards_us=%lld body_us=%lld prepare_us=%lld submit_us=%lld response_us=%lld total_us=%lld submit_result=%d response_result=%d",
            static_cast<unsigned>(key),static_cast<unsigned>(hold_ms),static_cast<unsigned>(hold_wait_ms),static_cast<long long>(guarded_us-started_us),static_cast<long long>(body_us-guarded_us),
            static_cast<long long>(submit_started_us-body_us),static_cast<long long>(submit_finished_us-submit_started_us),
            static_cast<long long>(finished_us-response_started_us),static_cast<long long>(total_us),static_cast<int>(outcome),static_cast<int>(response_result));
    }
    int64_t started_us=0,guarded_us=0,body_us=0,submit_started_us=0,submit_finished_us=0,response_started_us=0;
    bool enabled=false,has_key=false;
    uint8_t key=0;
    uint16_t hold_ms=0,hold_wait_ms=0;
    esp_err_t outcome=ESP_OK,response_result=ESP_OK;
};
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
esp_err_t send(httpd_req_t *req,cJSON *root) {
    char *s=cJSON_PrintUnformatted(root);
    if(!s)return web::error(req,"Out of memory","503 Service Unavailable");
    auto e=web::json(req,s);cJSON_free(s);return e;
}
esp_err_t status(httpd_req_t *req) {
    auto s=hub->snapshot();Json root(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(root.get(),"version",esp_app_get_description()->version);
    cJSON_AddStringToObject(root.get(),"hostname",device_settings::hostname());
    cJSON_AddStringToObject(root.get(),"front_physical_address",device_settings::format_physical_address(device_settings::front_physical_address()).c_str());
    if(s.front_physical_address==0xffff)cJSON_AddNullToObject(root.get(),"determined_front_physical_address");
    else cJSON_AddStringToObject(root.get(),"determined_front_physical_address",device_settings::format_physical_address(s.front_physical_address).c_str());
    cJSON_AddNumberToObject(root.get(),"free_heap",esp_get_free_heap_size());
    cJSON_AddNumberToObject(root.get(),"free_internal_heap",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    wifi_ap_record_t access_point{};
    const bool wifi_connected=esp_wifi_sta_get_ap_info(&access_point)==ESP_OK;
    cJSON_AddBoolToObject(root.get(),"wifi_connected",wifi_connected);
    if(wifi_connected) {
        cJSON_AddNumberToObject(root.get(),"wifi_rssi",access_point.rssi);
        cJSON_AddNumberToObject(root.get(),"wifi_channel",access_point.primary);
    }
    cJSON_AddBoolToObject(root.get(),"ready",s.ready);
    cJSON_AddBoolToObject(root.get(),"discovering",s.discovering);
    cJSON_AddBoolToObject(root.get(),"activity_settling",s.activity_settling);
    cJSON_AddStringToObject(root.get(),"activity_error",s.activity_error.c_str());
    cJSON_AddNumberToObject(root.get(),"discovery_generation",s.discovery_generation);
    cJSON_AddNumberToObject(root.get(),"activity_generation",s.activity_generation);
    cJSON_AddNumberToObject(root.get(),"current_activity",s.current_activity);
    cJSON_AddNumberToObject(root.get(),"front_address",s.front_address);
    cJSON_AddNumberToObject(root.get(),"back_address",s.back_address);
    cJSON_AddNumberToObject(root.get(),"tx_errors",s.tx_errors);
    cJSON_AddNumberToObject(root.get(),"rx_dropped",s.rx_dropped);
    cJSON_AddStringToObject(root.get(),"hardware","Amity Board / Fin");
    auto *devices=cJSON_AddArrayToObject(root.get(),"devices");
    for(auto &d:s.devices) {
        auto *o=cJSON_CreateObject();cJSON_AddItemToArray(devices,o);
        cJSON_AddStringToObject(o,"name",d.name.c_str());cJSON_AddStringToObject(o,"bus",d.bus==amity::BusSide::Front?"front":"back");
        cJSON_AddNumberToObject(o,"address",d.address);cJSON_AddNumberToObject(o,"physical_address",d.physical_address);cJSON_AddNumberToObject(o,"type",d.type);
        cJSON_AddNumberToObject(o,"vendor_id",d.vendor_id);
    }
    return send(req,root.get());
}
esp_err_t remotes(httpd_req_t *req) {
    ble_remote_status_t s{};ble_remote_get_status(&s);Json root(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(root.get(),"boot_id",system_services::boot_id());
    cJSON_AddStringToObject(root.get(),"remote_generation",amity::system::format_uint64(remote_approvals.generation()).c_str());
    cJSON_AddBoolToObject(root.get(),"ready",s.ready);cJSON_AddBoolToObject(root.get(),"scanning",s.scanning);cJSON_AddBoolToObject(root.get(),"connected",s.connected);
    cJSON_AddBoolToObject(root.get(),"bonded",s.bonded);cJSON_AddBoolToObject(root.get(),"connecting",s.connecting);
    cJSON_AddBoolToObject(root.get(),"waiting",s.waiting);
    cJSON_AddBoolToObject(root.get(),"pairing",s.pairing);
    cJSON_AddBoolToObject(root.get(),"unpairing",s.unpairing);
    cJSON_AddBoolToObject(root.get(),"too_far",s.too_far);
    cJSON_AddNumberToObject(root.get(),"battery",s.battery_percent);if(s.charging<0)cJSON_AddNullToObject(root.get(),"charging");else cJSON_AddBoolToObject(root.get(),"charging",s.charging==1);cJSON_AddNumberToObject(root.get(),"last_error",s.last_error);cJSON_AddStringToObject(root.get(),"name",s.peer.name);
    auto *list=cJSON_AddArrayToObject(root.get(),"devices");ble_remote_peer_t peers[16]{};
    auto n=ble_remote_scan_results(peers,16);
    for(size_t i=0;i<n;++i) {
        auto *o=cJSON_CreateObject();cJSON_AddItemToArray(list,o);char address[18];auto *a=peers[i].address;
        snprintf(address,sizeof(address),"%02x:%02x:%02x:%02x:%02x:%02x",a[0],a[1],a[2],a[3],a[4],a[5]);
        cJSON_AddStringToObject(o,"address",address);cJSON_AddStringToObject(o,"name",peers[i].name);cJSON_AddNumberToObject(o,"type",peers[i].address_type);cJSON_AddNumberToObject(o,"rssi",peers[i].rssi);
    }
    return send(req,root.get());
}
esp_err_t matter(httpd_req_t *req) {
    amity_matter_status_t s{};auto e=amity_matter_get_status(&s);if(e!=ESP_OK)return web::error(req,esp_err_to_name(e),"503 Service Unavailable");
    Json root(cJSON_CreateObject(),cJSON_Delete);cJSON_AddBoolToObject(root.get(),"started",s.started);cJSON_AddBoolToObject(root.get(),"commissioning",s.commissioning_window);
    if(s.commissioning_window) {
        cJSON_AddStringToObject(root.get(),"manual_code",s.manual_code);cJSON_AddStringToObject(root.get(),"qr_payload",s.qr_payload);
        // Version 8 fits even a full 127-byte payload at medium error correction.
        uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(8)],code[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
        if(!qrcodegen_encodeText(s.qr_payload,temporary,code,qrcodegen_Ecc_MEDIUM,1,8,qrcodegen_Mask_AUTO,true))
            return web::error(req,"Cannot generate pairing QR code","500 Internal Server Error");
        auto *rows=cJSON_AddArrayToObject(root.get(),"qr_modules");
        const int size=qrcodegen_getSize(code);
        for(int y=0;y<size;++y) {
            std::string row(size,'0');
            for(int x=0;x<size;++x)if(qrcodegen_getModule(code,x,y))row[x]='1';
            cJSON_AddItemToArray(rows,cJSON_CreateString(row.c_str()));
        }
    }
    auto *list=cJSON_AddArrayToObject(root.get(),"fabrics");
    for(unsigned i=0;i<s.fabric_count&&i<AMITY_MATTER_MAX_FABRICS;++i){auto *o=cJSON_CreateObject();cJSON_AddItemToArray(list,o);cJSON_AddNumberToObject(o,"index",s.fabrics[i].index);cJSON_AddStringToObject(o,"label",s.fabrics[i].label);cJSON_AddNumberToObject(o,"vendor",s.fabrics[i].vendor_id);}
    return send(req,root.get());
}
esp_err_t homekit(httpd_req_t *req) {
    const auto s=amity_homekit::status();Json root(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddBoolToObject(root.get(),"started",s.started);cJSON_AddBoolToObject(root.get(),"available",s.available);
    cJSON_AddBoolToObject(root.get(),"pairing",s.pairing);cJSON_AddBoolToObject(root.get(),"busy",s.busy);
    cJSON_AddBoolToObject(root.get(),"paired",s.paired);cJSON_AddNumberToObject(root.get(),"controllers",s.controllers);
    if(s.error!=ESP_OK)cJSON_AddStringToObject(root.get(),"error",esp_err_to_name(s.error));
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    if(s.pairing&&s.manual_code[0]&&s.qr_payload[0]) {
        cJSON_AddStringToObject(root.get(),"manual_code",s.manual_code);cJSON_AddStringToObject(root.get(),"qr_payload",s.qr_payload);
        uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(4)],code[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
        if(!qrcodegen_encodeText(s.qr_payload,temporary,code,qrcodegen_Ecc_MEDIUM,1,4,qrcodegen_Mask_AUTO,true))
            return web::error(req,"Cannot generate HomeKit QR code","500 Internal Server Error");
        auto *rows=cJSON_AddArrayToObject(root.get(),"qr_modules");const int size=qrcodegen_getSize(code);
        for(int y=0;y<size;++y){std::string row(size,'0');for(int x=0;x<size;++x)if(qrcodegen_getModule(code,x,y))row[x]='1';cJSON_AddItemToArray(rows,cJSON_CreateString(row.c_str()));}
    }
    return send(req,root.get());
}
esp_err_t api(httpd_req_t *req) {
    if (!ui_enabled.load()) return web::error(req,"Administration is available on the original Amity only.","403 Forbidden");
    KeyRequestTiming key_timing(req->method==HTTP_POST&&!strcmp(req->uri,"/api/key"));
    if(req->method==HTTP_GET&&req->content_len)return web::error(req,"GET requests cannot have a body");
    std::string path=req->uri;
    if(path=="/api/ready"&&req->method==HTTP_GET) {
        // Only this read-only, credential-free health response is available
        // across origins while the browser follows a changed hostname.
        httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
        Json root(cJSON_CreateObject(),cJSON_Delete);
        cJSON_AddStringToObject(root.get(),"device_id",hub->octopus_id().str().c_str());
        cJSON_AddStringToObject(root.get(),"boot_id",system_services::boot_id());
        cJSON_AddStringToObject(root.get(),"hostname",device_settings::hostname());
        cJSON_AddBoolToObject(root.get(),"ready",system_services::firmware_ready());
        return send(req,root.get());
    }
    if(path=="/api/setup") {
        if(req->method==HTTP_GET) {
            Json root(cJSON_CreateObject(),cJSON_Delete);
            cJSON_AddBoolToObject(root.get(),"required",web::setup_required());
            cJSON_AddStringToObject(root.get(),"hostname",device_settings::hostname());
            cJSON_AddStringToObject(root.get(),"version",esp_app_get_description()->version);
            char image_identity[65]{};
            const auto *description=esp_app_get_description();
            for(size_t i=0;i<sizeof(description->app_elf_sha256);++i)
                snprintf(image_identity+2*i,3,"%02x",description->app_elf_sha256[i]);
            cJSON_AddStringToObject(root.get(),"image_elf_sha256",image_identity);
            cJSON_AddBoolToObject(root.get(),"update_manifest_required",true);
            cJSON_AddStringToObject(root.get(),"boot_id",system_services::boot_id());
            cJSON_AddBoolToObject(root.get(),"ready",system_services::firmware_ready());
            return send(req,root.get());
        }
        if(req->method==HTTP_POST) {
            if(!web::setup_required())return web::error(req,"Password already set.","409 Conflict");
            // JSON requires a same-origin request or a successful CORS preflight.
            char content_type[64]{};
            if(httpd_req_get_hdr_value_str(req,"Content-Type",content_type,sizeof(content_type))!=ESP_OK ||
               (strcmp(content_type,"application/json") && strncmp(content_type,"application/json;",17)))
                return web::error(req,"JSON required","415 Unsupported Media Type");
            std::string data;if(!web::body(req,data,512))return ESP_FAIL;
            Json root(parse_json_input(data),cJSON_Delete);auto *password=cJSON_GetObjectItemCaseSensitive(root.get(),"password");
            if(!cJSON_IsString(password))return web::error(req,"Password required");
            auto e=web::setup_password(password->valuestring);
            if(e==ESP_ERR_INVALID_ARG)return web::error(req,"Enter a password of no more than 128 bytes.");
            if(e!=ESP_OK)return web::error(req,"Could not save password. Please try again.","500 Internal Server Error");
            return web::json(req,"{\"ok\":true}");
        }
    }
    if(path=="/api/login" && req->method==HTTP_POST) {
        std::string data;if(!web::body(req,data,512))return ESP_FAIL;
        Json root(parse_json_input(data),cJSON_Delete);auto *password=cJSON_GetObjectItemCaseSensitive(root.get(),"password");std::string token;
        auto retry_after=web::login_retry_after();
        char retry_value[12];
        auto retry_header=[&](unsigned seconds){snprintf(retry_value,sizeof(retry_value),"%u",seconds);httpd_resp_set_hdr(req,"Retry-After",retry_value);};
        if(retry_after) {retry_header(retry_after);return web::error(req,"Please wait before trying again.","429 Too Many Requests");}
        if(!cJSON_IsString(password)||!web::login(password->valuestring,token)) {
            retry_header(web::login_retry_after());
            return web::error(req,"The password is incorrect.","401 Unauthorized");
        }
        return web::json(req,std::string("{\"token\":\"")+token+"\"}");
    }
    if(!web::authenticated(req))return web::error(req,"Login required","401 Unauthorized");
    if(device_settings::reset_pending())return web::error(req,"Factory reset is pending. Restarting.","503 Service Unavailable");
    if(configuration_is_passive()&&req->method==HTTP_POST&&path!="/api/logout"&&path!="/api/password"&&path!="/api/firmware"&&path!="/api/reboot"&&path!="/api/updates/validate")
        return web::error(req,"Configuration changes are unavailable while Amity is passive or updating.","403 Forbidden");
    esp_err_t administration_result=ESP_OK;if(activity_editor_request(*hub,req,path,administration_result))return administration_result;
    if(admin_request(req,path,administration_result))return administration_result;
    key_timing.guards_complete();
    if(req->method==HTTP_GET) {
        if(path=="/api/status")return status(req);
        if(path=="/api/remotes")return remotes(req);
        if(path=="/api/matter")return matter(req);
        if(path=="/api/homekit")return homekit(req);
        if(path=="/api/logs") {
            return system_services::download_logs(req,hub->octopus_id());
        }
    }
    if(req->method==HTTP_POST) {
        if(path=="/api/firmware")return system_services::upload_firmware(req);

        bool supported=false;
        for(const auto* command:{"/api/key","/api/remotes/scan","/api/remotes/pair","/api/remotes/unpair","/api/remotes/stop",
            "/api/matter/open","/api/matter/close","/api/matter/remove","/api/homekit/open","/api/homekit/close",
            "/api/homekit/remove","/api/factory-reset","/api/password","/api/logout","/api/reboot"})supported|=path==command;
        if(!supported)return web::error(req,"Unknown endpoint","404 Not Found");
        std::string data;if(!web::body(req,data))return ESP_FAIL;
        key_timing.body_complete();
        Json root(parse_json_input(data.empty()?"{}":data),cJSON_Delete);
        if(!root)return web::error(req,"Invalid JSON");
        esp_err_t e=ESP_OK;
        auto int_arg=[&](const char *key,int low,int high,int &out){auto *v=cJSON_GetObjectItemCaseSensitive(root.get(),key);if(!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<low||v->valueint>high)return false;out=v->valueint;return true;};
        if(path=="/api/key") {
            KeyRequest key;if(!parse_key_request(root.get(),key))return web::error(req,"Expected a key from 0 to 255 and duration_ms from 0 to 5000");
            key_timing.submitting(key);
            e=submit_key_request(*hub,key,[&key_timing](uint16_t duration){key_timing.holding(duration);vTaskDelay(pdMS_TO_TICKS(duration-1)+1);})?ESP_OK:ESP_ERR_NO_MEM;
            key_timing.submitted(e);
        }
        else if(path=="/api/remotes/scan")e=ble_remote_scan(20);
        // TIMEOUT: the abandoned connection is still closing; status reports it.
        else if(path=="/api/remotes/stop"){e=ble_remote_stop_pairing();if(e==ESP_ERR_TIMEOUT)e=ESP_OK;}
        else if(path=="/api/remotes/unpair"||path=="/api/remotes/pair") {
            LocalRemoteRequest approval;
            if(!parse_local_remote_request(root.get(),path=="/api/remotes/pair",approval))return web::error(req,"Review the remote's current pairing state before continuing","400 Bad Request");
            const auto boot=amity::system::BootId::parse(system_services::boot_id()).value_or(amity::system::BootId{});
            const auto admitted=remote_approvals.execute(boot,approval,[&]{return approval.pair?(approval.targeted?ble_remote_pair(approval.address.data(),approval.address_type):ble_remote_start_pairing(20)):ble_remote_unpair();});
            if(!admitted.accepted)return web::error(req,"The remote's pairing state changed. Review it before continuing.","409 Conflict");
            e=admitted.result;
            if(e!=ESP_OK)return web::error(req,esp_err_to_name(e),"409 Conflict");
            Json response(cJSON_CreateObject(),cJSON_Delete);cJSON_AddBoolToObject(response.get(),"saved",true);cJSON_AddBoolToObject(response.get(),"admitted",true);cJSON_AddBoolToObject(response.get(),"duplicate",admitted.duplicate);cJSON_AddStringToObject(response.get(),"remote_generation",amity::system::format_uint64(admitted.generation).c_str());return send(req,response.get());
        }
        else if(path=="/api/matter/open")e=amity_matter_open_commissioning(300);
        else if(path=="/api/matter/close")e=amity_matter_close_commissioning();
        else if(path=="/api/matter/remove") {int index;if(!int_arg("index",1,254,index))return web::error(req,"Invalid fabric index");e=amity_matter_remove_fabric(index);}
        else if(path=="/api/homekit/open")e=amity_homekit::open_pairing();
        else if(path=="/api/homekit/close")e=amity_homekit::close_pairing();
        else if(path=="/api/homekit/remove") {
            if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"confirm")))return web::error(req,"HomeKit removal confirmation required");
            e=amity_homekit::remove_pairings();
        }
        else if(path=="/api/factory-reset") {
            if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"confirm")))return web::error(req,"Factory reset confirmation required");
            e=device_settings::schedule_factory_reset();
            if(e!=ESP_OK)return web::error(req,"Could not schedule factory reset.","500 Internal Server Error");
            auto response=web::json(req,"{\"rebooting\":true}");
            system_services::reboot_later();return response;
        }
        else if(path=="/api/password") {auto *p=cJSON_GetObjectItemCaseSensitive(root.get(),"password");if(!cJSON_IsString(p))return web::error(req,"Password required");e=web::change_password(p->valuestring);}
        else if(path=="/api/logout")web::logout(req);
        else if(path=="/api/reboot")system_services::reboot_later();
        else return web::error(req,"Unknown endpoint","404 Not Found");
        key_timing.responding();
        return key_timing.finish_response(e==ESP_OK?web::json(req,"{\"ok\":true}"):web::error(req,esp_err_to_name(e),"409 Conflict"));
    }
    return web::error(req,"Unknown endpoint","404 Not Found");
}
extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t html_end[] asm("_binary_index_html_end");
extern const uint8_t js_start[] asm("_binary_app_js_start");
extern const uint8_t js_end[] asm("_binary_app_js_end");
extern const uint8_t css_start[] asm("_binary_style_css_start");
extern const uint8_t css_end[] asm("_binary_style_css_end");
extern const uint8_t system_js_start[] asm("_binary_system_js_start");
extern const uint8_t system_js_end[] asm("_binary_system_js_end");
esp_err_t asset(httpd_req_t *req) {
    if (!ui_enabled.load()) return web::error(req,"Administration is available on the original Amity only.","403 Forbidden");
    if(req->content_len)return web::error(req,"GET requests cannot have a body");
    const uint8_t *begin=html_start,*end=html_end;const char *mime="text/html; charset=utf-8";
    if(!strcmp(req->uri,"/app.js")){begin=js_start;end=js_end;mime="text/javascript";}
    if(!strcmp(req->uri,"/style.css")){begin=css_start;end=css_end;mime="text/css";}
    if(!strcmp(req->uri,"/system.js")){begin=system_js_start;end=system_js_end;mime="text/javascript";}
    httpd_resp_set_type(req,mime);httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Cache-Control","no-cache");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'self'; connect-src 'self' https://*.local; script-src 'self'; style-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
    return web::response(req,reinterpret_cast<const char *>(begin),end-begin);
}
}
esp_err_t load(amity::SystemControl &service,bool defer_configuration) {
    hub=&service;
    return load_configuration(service,defer_configuration);
}

esp_err_t start(amity::SystemControl &service,const char *hostname,const char *initial_auth) {
    std::lock_guard<std::mutex> guard(server_mutex);
    if(ui_revoked.load() || server) return ESP_ERR_INVALID_STATE;
    const auto role=amity::consortium::runtime_status();
    if(!role.coordinator || !role.authority_ready || role.member || role.phase=="retired" || role.phase=="recovery_required") return ESP_ERR_INVALID_STATE;
    hub=&service;auto e=web::start(server,std::string(hostname)+".local",initial_auth);if(e!=ESP_OK)return e;
    for(auto *path:{"/","/app.js","/system.js","/style.css"}) {e=web::route(server,path,HTTP_GET,asset);if(e!=ESP_OK)return e;}
    e=web::route(server,"/api/*",HTTP_GET,api);if(e!=ESP_OK)return e;
    e=web::route(server,"/api/*",HTTP_POST,api);
    if(e==ESP_OK)ui_enabled.store(true);
    return e;
}
esp_err_t stop() {
    // Revoke handlers before closing sockets, including already accepted ones.
    ui_revoked.store(true); ui_enabled.store(false);
    std::lock_guard<std::mutex> guard(server_mutex);
    if(!server)return ESP_OK;
    system_services::stop_log_downloads();
    const auto result=httpd_ssl_stop(server);
    if(result==ESP_OK)server=nullptr;
    return result;
}

}
