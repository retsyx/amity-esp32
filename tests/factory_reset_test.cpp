/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "device_settings.hpp"
#include "nvs_flash.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
using Bytes=std::vector<uint8_t>;
using Space=std::map<std::string,Bytes>;
using Store=std::map<std::string,Space>;
Store store;
std::map<unsigned,std::string> handles;
unsigned next_handle=1;
int steps=0,cut=0;
bool initialized=false;
bool check_concurrent_request=false;
esp_err_t commit_error=ESP_OK;
void step(){if(++steps==cut)throw std::runtime_error("power loss");}
esp_err_t nvs_open_from_partition(const char *partition,const char *name,nvs_open_mode_t mode,nvs_handle_t *out){
    step();if(std::string(partition)=="nvs"&&!initialized)return ESP_ERR_NVS_NOT_INITIALIZED;
    std::string key=std::string(partition)+"/"+name;
    if(!store.count(key)&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;
    store[key];*out=next_handle++;handles[*out]=key;return ESP_OK;
}
esp_err_t nvs_open(const char *name,nvs_open_mode_t mode,nvs_handle_t *out){return nvs_open_from_partition("nvs",name,mode,out);}
void nvs_close(nvs_handle_t h){handles.erase(h);}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size){
    step();auto &s=store.at(handles.at(h));if(!s.count(key))return ESP_ERR_NVS_NOT_FOUND;
    auto &value=s.at(key);if(out){if(*size<value.size())return ESP_ERR_INVALID_SIZE;memcpy(out,value.data(),value.size());}*size=value.size();return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size){step();const auto *p=static_cast<const uint8_t *>(data);store.at(handles.at(h))[key]=Bytes(p,p+size);return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *out){size_t n=1;return nvs_get_blob(h,key,out,&n);}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value){return nvs_set_blob(h,key,&value,1);}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key){step();return store.at(handles.at(h)).erase(key)?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_erase_all(nvs_handle_t h){step();auto &s=store.at(handles.at(h));while(!s.empty()){step();s.erase(s.begin());}return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){
    step();
    if(check_concurrent_request)assert(device_settings::schedule_factory_reset()==ESP_ERR_INVALID_STATE);
    return commit_error;
}
esp_err_t nvs_flash_init(){step();initialized=true;return ESP_OK;}
esp_err_t nvs_flash_deinit(){step();if(!initialized)return ESP_ERR_NVS_NOT_INITIALIZED;initialized=false;return ESP_OK;}
esp_err_t nvs_flash_erase(){step();for(auto &[key,space]:store)if(key.rfind("nvs/",0)==0)while(!space.empty()){step();space.erase(space.begin());}return ESP_OK;}
int main(){
    unsigned interruptions=0;
    store={{"nvs/wifi",{{"ssid",{1,2}},{"password",{3,4}}}},
           {"nvs/chip-config",{{"fabric",{7}}}},{"nvs/nimble",{{"bond",{8}}}},
           {"nvs/amity_matter",{{"commissioning",Bytes(44,0x42)}}},
           {"amity_cfg/amity",{{"front_physical",{5}},{"hostname",{1}},{"admin_auth",{2}},{"tls_cert",{3}},{"config",{4}}}},
           {"amity_factory/raw",{{"provisioning",{4,5,6}}}},
           {"nvs_keys/raw",{{"key",{10,11}}}},
           {"ota_0/raw",{{"firmware",{12,13}}}},
           {"ota_1/raw",{{"firmware",{14,15}}}}};
    // Normal boots never erase mutable storage without a reset marker.
    const auto untouched=store;
    assert(device_settings::finish_factory_reset()==ESP_OK);
    assert(store==untouched);
    store["amity_cfg/amity_reset"]["pending"]={2};
    const auto invalid_marker=store;
    assert(device_settings::finish_factory_reset()==ESP_ERR_INVALID_STATE);
    assert(store==invalid_marker);
    store.erase("amity_cfg/amity_reset");
    // Failed persistence must release the guard and allow another attempt.
    commit_error=ESP_ERR_INVALID_STATE;check_concurrent_request=true;
    assert(device_settings::schedule_factory_reset()==ESP_ERR_INVALID_STATE);
    assert(!device_settings::reset_pending());
    commit_error=ESP_OK;
    assert(device_settings::schedule_factory_reset()==ESP_OK);
    check_concurrent_request=false;
    assert(device_settings::reset_pending());
    assert(device_settings::schedule_factory_reset()==ESP_ERR_INVALID_STATE);
    auto baseline=store;
    auto verify=[&]{
        for(const auto &[key,space]:store)if(key.rfind("nvs/",0)==0)assert(space.empty());
        assert(store["amity_cfg/amity"].empty());
        assert(store["amity_cfg/amity_reset"].empty());
        for(const auto &key:{"amity_factory/raw","nvs_keys/raw","ota_0/raw","ota_1/raw"})
            assert(store.at(key)==untouched.at(key));
    };
    steps=0;assert(device_settings::finish_factory_reset()==ESP_OK);verify();const int total=steps;
    assert(!device_settings::reset_pending());
    for(int i=1;i<=total;++i){
        store=baseline;handles.clear();initialized=false;steps=0;cut=i;
        try{device_settings::finish_factory_reset();}catch(const std::runtime_error &){++interruptions;}
        // Reboot with persisted flash and no open NVS handles or network tasks.
        handles.clear();initialized=false;cut=0;
        assert(device_settings::finish_factory_reset()==ESP_OK);verify();
        assert(device_settings::finish_factory_reset()==ESP_OK);verify();
    }
    std::cout<<"PASS factory reset resumes after "<<interruptions<<" simulated power interruptions; factory data, NVS keys and firmware retained\n";
}
