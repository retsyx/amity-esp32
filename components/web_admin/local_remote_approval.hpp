// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system/model.hpp"
#include "cJSON.h"
#include <array>
#include <limits>
#include <set>

namespace web_admin {
struct LocalRemoteRequest {
    amity::system::MessageId id;
    amity::system::BootId boot;
    uint64_t generation=0;
    bool pair=false;
    bool operator==(const LocalRemoteRequest& other)const {
        return id==other.id&&boot==other.boot&&generation==other.generation&&pair==other.pair;
    }
};
inline bool parse_local_remote_request(const cJSON* root,bool pair,LocalRemoteRequest& out){
    if(!cJSON_IsObject(root))return false;
    std::set<std::string> keys;
    for(const auto* field=root->child;field;field=field->next){
        if(!field->string||!keys.insert(field->string).second)return false;
        const std::string name=field->string;
        if(name!="request_id"&&name!="expected_boot_id"&&name!="expected_remote_generation")return false;
    }
    const auto text=[&](const char* key){const auto* field=cJSON_GetObjectItemCaseSensitive(root,key);return cJSON_IsString(field)&&field->valuestring?std::string(field->valuestring):std::string{};};
    const auto id=amity::system::MessageId::parse(text("request_id"));const auto boot=amity::system::BootId::parse(text("expected_boot_id"));const auto generation=amity::system::parse_uint64(text("expected_remote_generation"));
    if(!id||!boot||!generation)return false;
    LocalRemoteRequest request;request.id=*id;request.boot=*boot;request.generation=*generation;request.pair=pair;
    out=request;return true;
}
struct LocalRemoteAdmission {bool accepted=false,duplicate=false;int result=0;uint64_t generation=0;};
// Administration-task owned. Cache eviction cannot make an old approval usable:
// its boot-local generation was consumed before the BLE worker was called.
class LocalRemoteApprovalBook {
public:
    static constexpr size_t capacity=32;
    uint64_t generation()const{return generation_;}
    template<class Perform>LocalRemoteAdmission execute(amity::system::BootId trusted_boot,const LocalRemoteRequest& request,Perform perform){
        if(!trusted_boot||!request.id||request.boot!=trusted_boot)return {};
        for(const auto& entry:entries_)if(entry.used&&entry.request.id==request.id){
            if(!(entry.request==request))return {};
            return {true,true,entry.result,generation_};
        }
        if(request.generation!=generation_||generation_==std::numeric_limits<uint64_t>::max())return {};
        ++generation_; // Consumed even if admission or the radio operation fails.
        const int result=perform();
        entries_[next_]={request,result,true};next_=(next_+1)%capacity;
        return {true,false,result,generation_};
    }
private:
    struct Entry {LocalRemoteRequest request;int result=0;bool used=false;};
    std::array<Entry,capacity> entries_{};size_t next_=0;uint64_t generation_=0;
};
}
