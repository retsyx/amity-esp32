/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "config_store.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>

std::map<std::string,std::string> blobs;
std::map<std::string,std::string> strings{{"string","old configuration"}};
esp_err_t string_lookup_error=ESP_ERR_NVS_NOT_FOUND,commit_error=ESP_OK;
unsigned open_handles=0,commits=0,string_reads=0;

esp_err_t nvs_flash_init_partition(const char *partition){assert(std::string(partition)=="amity_cfg");return ESP_OK;}
esp_err_t nvs_open_from_partition(const char *partition,const char *space,nvs_open_mode_t,nvs_handle_t *out){
    assert(std::string(partition)=="amity_cfg"&&std::string(space)=="amity");
    *out=++open_handles;return ESP_OK;
}
void nvs_close(nvs_handle_t){assert(open_handles==1);--open_handles;}
esp_err_t nvs_get_blob(nvs_handle_t,const char *key,void *out,size_t *size){
    if(strings.count(key))return string_lookup_error;
    const auto entry=blobs.find(key);if(entry==blobs.end())return ESP_ERR_NVS_NOT_FOUND;
    const auto &data=entry->second;
    if(out){if(*size<data.size())return ESP_ERR_INVALID_SIZE;std::memcpy(out,data.data(),data.size());}
    *size=data.size();return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t,const char *key,char *out,size_t *size){
    ++string_reads;const auto entry=strings.find(key);if(entry==strings.end())return ESP_ERR_NVS_NOT_FOUND;
    const auto &data=entry->second;const auto required=data.size()+1;
    if(out){if(*size<required)return ESP_ERR_INVALID_SIZE;std::memcpy(out,data.c_str(),required);}
    *size=required;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t,const char *key,const void *data,size_t size){
    blobs[key]=std::string(static_cast<const char *>(data),size);return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t){++commits;return commit_error;}

int main(){
    assert(config_store::init()==ESP_OK);
    std::string result="keep existing value";
    assert(config_store::read("missing",result)==ESP_ERR_NVS_NOT_FOUND);
    assert(result=="keep existing value"&&open_handles==0);
    // Depending on the NVS lookup path, a string can report either error.
    for(const auto error:{ESP_ERR_NVS_NOT_FOUND,ESP_ERR_NVS_TYPE_MISMATCH}){
        string_lookup_error=error;
        assert(config_store::read("string",result)==error);
        assert(result=="keep existing value"&&open_handles==0&&string_reads==0);
        assert(strings.at("string")=="old configuration");
    }
    for(const auto &value:{std::string{},std::string("config\0bytes",12),std::string(32768,'c')}){
        assert(config_store::write("config",value)==ESP_OK);
        assert(config_store::read("config",result)==ESP_OK);
        assert(result==value&&open_handles==0);
    }
    const auto before=commits;
    assert(config_store::write("config",std::string(32769,'x'))==ESP_ERR_INVALID_SIZE);
    assert(commits==before&&blobs["config"]==std::string(32768,'c'));
    blobs["oversized"]=std::string(65537,'x');result="keep existing value";
    assert(config_store::read("oversized",result)==ESP_ERR_INVALID_SIZE);
    assert(result=="keep existing value"&&open_handles==0);
    commit_error=ESP_FAIL;
    assert(config_store::write("config","new")==ESP_FAIL&&open_handles==0);
    std::cout<<"PASS configuration blobs round-trip full supported sizes; strings rejected without changing data\n";
}
