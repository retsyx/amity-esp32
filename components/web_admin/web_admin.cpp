/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "web_admin.hpp"
#include "web.hpp"
#include "config_store.hpp"
#include "device_settings.hpp"
#include "system_services.hpp"
#include "amity_matter.h"
#include "homekit.hpp"
#include "ble_remote.h"
#include "cJSON.h"
#include "json_input.hpp"
#include "qrcodegen.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <memory>
namespace web_admin {
namespace {
amity::HubService *hub;
httpd_handle_t server;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
const char empty_config[]="{\"version\":1,\"activities\":[]}";
esp_err_t send(httpd_req_t *req,cJSON *root) {
    char *s=cJSON_PrintUnformatted(root);
    if(!s)return web::error(req,"Out of memory","503 Service Unavailable");
    auto e=web::json(req,s);cJSON_free(s);return e;
}
void inputs(const Configuration &c) {
    amity_matter_input_t in[16]{};
    for(size_t i=0;i<c.activities.size();++i) {in[i].index=i;snprintf(in[i].name,sizeof(in[i].name),"%s",c.activities[i].name.c_str());}
    amity_matter_set_inputs(in,c.activities.size());
}
esp_err_t apply(const std::string &data,bool persist,std::string &error) {
    Configuration c;
    if(!parse_configuration(data,c,error))return ESP_ERR_INVALID_ARG;
    auto e=hub->configure_with_commit(c.activities,c.quirks,[&] {
        return persist ? config_store::write("config",data) : ESP_OK;
    });
    if(e!=ESP_OK) {error="Configuration could not be committed";return e;}
    inputs(c);return ESP_OK;
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
    cJSON_AddNumberToObject(root.get(),"largest_internal_block",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
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
    cJSON_AddNumberToObject(root.get(),"discovery_generation",s.discovery_generation);
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
    if(req->method==HTTP_GET&&req->content_len)return web::error(req,"GET requests cannot have a body");
    std::string path=req->uri;
    if(path=="/api/setup") {
        if(req->method==HTTP_GET) {
            Json root(cJSON_CreateObject(),cJSON_Delete);
            cJSON_AddBoolToObject(root.get(),"required",web::setup_required());
            cJSON_AddStringToObject(root.get(),"hostname",device_settings::hostname());
            cJSON_AddStringToObject(root.get(),"version",esp_app_get_description()->version);
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
    if(req->method==HTTP_GET) {
        if(path=="/api/status")return status(req);
        if(path=="/api/remotes")return remotes(req);
        if(path=="/api/matter")return matter(req);
        if(path=="/api/homekit")return homekit(req);
        if(path=="/api/config"||path=="/api/backup") {
            std::string data;auto e=config_store::read("config",data);if(e==ESP_ERR_NVS_NOT_FOUND)data=empty_config;else if(e!=ESP_OK)return web::error(req,"Cannot read configuration","500 Internal Server Error");
            httpd_resp_set_hdr(req,"X-Amity-Configured",e==ESP_ERR_NVS_NOT_FOUND?"false":"true");
            if(path=="/api/backup")httpd_resp_set_hdr(req,"Content-Disposition","attachment; filename=amity-config.json");
            return web::json(req,data);
        }
        if(path=="/api/logs") {
            auto data=system_services::logs();httpd_resp_set_type(req,"text/plain");httpd_resp_set_hdr(req,"Content-Disposition","attachment; filename=amity.log");httpd_resp_set_hdr(req,"Cache-Control","no-store");
            return httpd_resp_send(req,data.data(),data.size());
        }
    }
    if(req->method==HTTP_POST) {
        if(path=="/api/firmware")return system_services::upload_firmware(req);
        std::string data;if(!web::body(req,data))return ESP_FAIL;
        if(path=="/api/config"||path=="/api/restore") {
            std::string error;auto e=apply(data,true,error);
            return e==ESP_OK?web::json(req,"{\"saved\":true}"):web::error(req,error.empty()?esp_err_to_name(e):error.c_str());
        }
        Json root(parse_json_input(data.empty()?"{}":data),cJSON_Delete);
        if(!root)return web::error(req,"Invalid JSON");
        esp_err_t e=ESP_OK;
        auto int_arg=[&](const char *key,int low,int high,int &out){auto *v=cJSON_GetObjectItemCaseSensitive(root.get(),key);if(!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<low||v->valueint>high)return false;out=v->valueint;return true;};
        if(path=="/api/discover")e=hub->discover()?ESP_OK:ESP_ERR_NO_MEM;
        else if(path=="/api/activity") {int index;if(!int_arg("index",-1,15,index)||index>=static_cast<int>(hub->snapshot().activities.size()))return web::error(req,"Invalid activity index");e=hub->request_activity(index);if(e==ESP_ERR_TIMEOUT)return web::error(req,"System is settling. Try again shortly.","409 Conflict");}
        else if(path=="/api/key") {int key;if(!int_arg("key",0,255,key))return web::error(req,"Invalid key");e=hub->press_key(key)?ESP_OK:ESP_ERR_NO_MEM;}
        else if(path=="/api/remotes/scan")e=ble_remote_scan(20);
        else if(path=="/api/remotes/unpair")e=ble_remote_unpair();
        else if(path=="/api/remotes/pair") {
            auto *address=cJSON_GetObjectItemCaseSensitive(root.get(),"address");int type;
            if(!address)e=ble_remote_start_pairing(20);
            else {
            if(!cJSON_IsString(address)||strlen(address->valuestring)!=17||!int_arg("type",0,3,type))return web::error(req,"Invalid BLE address");
            unsigned bytes[6];char tail;
            if(sscanf(address->valuestring,"%2x:%2x:%2x:%2x:%2x:%2x%c",&bytes[0],&bytes[1],&bytes[2],&bytes[3],&bytes[4],&bytes[5],&tail)!=6)return web::error(req,"Invalid BLE address");
            uint8_t peer[6];for(unsigned i=0;i<6;++i)peer[i]=bytes[i];e=ble_remote_pair(peer,type);
            }
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
        else if(path=="/api/front-physical-address") {
            auto *value=cJSON_GetObjectItemCaseSensitive(root.get(),"address");
            uint16_t address;
            if(!cJSON_IsString(value)||!device_settings::parse_front_physical_address(value->valuestring,address))
                return web::error(req,"Use a valid dotted hexadecimal physical address, such as 2.0.0.0, or leave blank for 1.0.0.0.");
            e=device_settings::save_front_physical_address(value->valuestring);
            if(e!=ESP_OK)return web::error(req,"Could not save front physical address.","500 Internal Server Error");
            auto response=web::json(req,std::string("{\"front_physical_address\":\"")+device_settings::format_physical_address(address)+"\",\"rebooting\":true}");
            system_services::reboot_later();return response;
        }
        else if(path=="/api/hostname") {
            auto *value=cJSON_GetObjectItemCaseSensitive(root.get(),"hostname");
            if(!cJSON_IsString(value))return web::error(req,"Hostname required");
            std::string name=value->valuestring;
            if(!device_settings::normalize_hostname(name))return web::error(req,"Use 1–32 letters, digits or hyphens, without a leading or trailing hyphen.");
            e=device_settings::save_hostname(name);
            if(e!=ESP_OK)return web::error(req,"Could not save hostname.","500 Internal Server Error");
            auto response=web::json(req,std::string("{\"hostname\":\"")+name+"\",\"rebooting\":true}");
            system_services::reboot_later();return response;
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
        return e==ESP_OK?web::json(req,"{\"ok\":true}"):web::error(req,esp_err_to_name(e),"409 Conflict");
    }
    return web::error(req,"Unknown endpoint","404 Not Found");
}
extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t html_end[] asm("_binary_index_html_end");
extern const uint8_t js_start[] asm("_binary_app_js_start");
extern const uint8_t js_end[] asm("_binary_app_js_end");
extern const uint8_t css_start[] asm("_binary_style_css_start");
extern const uint8_t css_end[] asm("_binary_style_css_end");
esp_err_t asset(httpd_req_t *req) {
    if(req->content_len)return web::error(req,"GET requests cannot have a body");
    const uint8_t *begin=html_start,*end=html_end;const char *mime="text/html; charset=utf-8";
    if(!strcmp(req->uri,"/app.js")){begin=js_start;end=js_end;mime="text/javascript";}
    if(!strcmp(req->uri,"/style.css")){begin=css_start;end=css_end;mime="text/css";}
    httpd_resp_set_type(req,mime);httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Cache-Control","no-cache");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
    return httpd_resp_send(req,reinterpret_cast<const char *>(begin),end-begin);
}
}
esp_err_t load(amity::HubService &service) {hub=&service;std::string data,error;auto e=config_store::read("config",data);if(e==ESP_ERR_NVS_NOT_FOUND)data=empty_config;else if(e!=ESP_OK)return e;return apply(data,false,error);}
esp_err_t start(amity::HubService &service,const char *hostname,const char *initial_auth) {
    hub=&service;auto e=web::start(server,std::string(hostname)+".local",initial_auth);if(e!=ESP_OK)return e;
    for(auto *path:{"/","/app.js","/style.css"}) {e=web::route(server,path,HTTP_GET,asset);if(e!=ESP_OK)return e;}
    e=web::route(server,"/api/*",HTTP_GET,api);if(e!=ESP_OK)return e;
    return web::route(server,"/api/*",HTTP_POST,api);
}
}
