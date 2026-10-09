/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "configuration.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstring>
#include <set>
#include <initializer_list>
namespace web_admin {
namespace {
bool text(cJSON *object,const char *key,std::string &out,bool required=false) {
    auto *v=cJSON_GetObjectItemCaseSensitive(object,key);
    if(!v || cJSON_IsNull(v)) {out.clear();return !required;}
    if(!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring)>63 || (required && !*v->valuestring)) return false;
    out=v->valuestring;
    for(unsigned char c:out) if(c<32)return false;
    return true;
}
bool integer(cJSON *v,int low,int high) {return cJSON_IsNumber(v)&&std::isfinite(v->valuedouble)&&v->valuedouble==v->valueint&&v->valueint>=low&&v->valueint<=high;}
bool only_keys(cJSON *object,std::initializer_list<const char *> allowed) {
    if(!cJSON_IsObject(object))return false;
    for(auto *item=object->child;item;item=item->next) {
        bool found=false;
        for(auto *key:allowed)if(item->string&&!strcmp(item->string,key)){found=true;break;}
        if(!found)return false;
    }
    return true;
}
bool unique_keys(cJSON *v) {
    if(cJSON_IsObject(v)) {std::set<std::string> keys;for(auto *c=v->child;c;c=c->next)if(!c->string||!keys.insert(c->string).second||!unique_keys(c))return false;}
    else if(cJSON_IsArray(v))for(auto *c=v->child;c;c=c->next)if(!unique_keys(c))return false;
    return true;
}
}
bool parse_configuration(const std::string &json,Configuration &config,std::string &error) {
    config={};
    if(json.size()>32768 || json.find('\0')!=std::string::npos) {error="Configuration is too large or contains NUL";return false;}
    // Bound parser recursion before cJSON sees input from the network.
    unsigned depth=0;bool quoted=false,escaped=false;
    for(char ch:json) {
        if(quoted) {if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='\"')quoted=false;}
        else if(ch=='\"')quoted=true;
        else if(ch=='{'||ch=='[') {if(++depth>8){error="Configuration nesting is too deep";return false;}}
        else if(ch=='}'||ch==']') {if(depth)--depth;}
    }
    auto *root=cJSON_ParseWithLengthOpts(json.c_str(),json.size()+1,nullptr,true);
    if(!root) {error="Invalid JSON";return false;}
    auto fail=[&](const char *s){error=s;cJSON_Delete(root);return false;};
    if(!cJSON_IsObject(root)||!unique_keys(root)||!integer(cJSON_GetObjectItemCaseSensitive(root,"version"),1,1))return fail("Expected configuration version 1 with unique keys");
    if(!only_keys(root,{"version","activities","quirks"}))return fail("Backup supports only activities and vendor quirks; device and Matter settings are excluded");
    auto *list=cJSON_GetObjectItemCaseSensitive(root,"activities");
    if(!cJSON_IsArray(list)||cJSON_GetArraySize(list)>16)return fail("Expected up to 16 activities");
    std::set<std::string> names;
    for(auto *item=list->child;item;item=item->next) {
        amity::Activity a;
        if(!only_keys(item,{"name","display","source","audio","switch"})||!text(item,"name",a.name,true)||!text(item,"display",a.display)||!text(item,"source",a.source)||!text(item,"audio",a.audio))return fail("Activity fields must be strings up to 63 bytes");
        if(!names.insert(a.name).second)return fail("Activity names must be unique");
        auto *sw=cJSON_GetObjectItemCaseSensitive(item,"switch");
        if(sw&&!cJSON_IsNull(sw)) {
            auto *input=cJSON_GetObjectItemCaseSensitive(sw,"input");
            if(!only_keys(sw,{"device","input"})||!text(sw,"device",a.switch_device,true)||!integer(input,0,255))return fail("Switch requires device and input 0..255");
            a.switch_input=input->valueint;
        }
        config.activities.push_back(std::move(a));
    }
    auto *quirks=cJSON_GetObjectItemCaseSensitive(root,"quirks");
    if(quirks) {
        if(!cJSON_IsArray(quirks)||cJSON_GetArraySize(quirks)>32)return fail("Expected up to 32 vendor quirks");
        for(auto *item=quirks->child;item;item=item->next) {
            if(!only_keys(item,{"vendor_id","power_on","power_off"}))return fail("Unknown quirk field");
            amity::Quirk q;auto *vendor=cJSON_GetObjectItemCaseSensitive(item,"vendor_id");
            if(!integer(vendor,0,0xffffff))return fail("Invalid quirk vendor ID");
            q.vendor_id=vendor->valueint;
            const std::pair<const char *,std::vector<uint8_t> *> payloads[]={{"power_on",&q.power_on},{"power_off",&q.power_off}};
            for(auto pair:payloads) {
                auto *bytes=cJSON_GetObjectItemCaseSensitive(item,pair.first);
                if(!bytes)continue;
                if(!cJSON_IsArray(bytes)||cJSON_GetArraySize(bytes)<1||cJSON_GetArraySize(bytes)>15)return fail("Quirk payload must contain 1..15 bytes");
                for(auto *byte=bytes->child;byte;byte=byte->next) {if(!integer(byte,0,255))return fail("Invalid quirk byte");pair.second->push_back(byte->valueint);}
            }
            config.quirks.push_back(std::move(q));
        }
    }
    cJSON_Delete(root);return true;
}
}
