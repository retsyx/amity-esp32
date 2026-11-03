// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "adapter_settings.hpp"
#include "amity/system_control.hpp"
#include "amity/consortium/runtime.hpp"
#include "device_settings.hpp"
#include "esp_timer.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace system_services::adapter_settings {
namespace {
namespace c=amity::consortium;
namespace m=amity::system;
amity::SystemControl* owner=nullptr;
std::mutex mutation;
struct Receipt {m::MessageId id;std::string fingerprint,response;c::Error outcome;};
std::vector<Receipt> receipts;
uint64_t now(){return static_cast<uint64_t>(esp_timer_get_time()/1000);}
const char* code(c::Error value){
    switch(value){
        case c::Error::none:return "";
        case c::Error::stale_boot:return "STALE_BOOT";
        case c::Error::transaction_conflict:case c::Error::stale_config:return "CONFLICT";
        case c::Error::expired:return "EXPIRED";
        case c::Error::storage_failure:return "STORAGE_FAILURE";
        case c::Error::queue_full:return "QUEUE_FULL";
        case c::Error::invalid_message:return "INVALID_MESSAGE";
        default:return "NOT_READY";
    }
}
std::string address(std::optional<uint16_t> value){return value&&*value!=0xffff?'"'+device_settings::format_physical_address(*value)+'"':"null";}
std::string result(m::MessageId id,c::Error error){return "{\"request_id\":\""+id.str()+"\",\"stage\":\""+(error==c::Error::none?"applied":"failed")+"\",\"error\":\""+code(error)+"\",\"snapshot\":"+(error==c::Error::none?snapshot():"null")+"}";}
struct Change {device_settings::AdapterDefaultChange preference;};
struct Request {m::BootId boot;uint64_t revision=0;std::vector<Change> changes;std::string fingerprint;};
bool parse(const c::Json& json,Request& request){
    if(!c::closed(json,{"expected_boot_id","expected_settings_revision","changes"})||
       !c::id(json.get("expected_boot_id"),request.boot)||!c::decimal(json.get("expected_settings_revision"),request.revision)||!request.revision)return false;
    const auto* changes=json.get("changes");
    if(!changes||changes->type!=c::Json::Type::array||changes->array.empty()||changes->array.size()>amity::max_tentacles)return false;
    uint8_t seen=0;
    for(const auto& item:changes->array){
        uint64_t id=0;std::string physical;uint16_t parsed=0;
        if(!c::closed(item,{"tentacle_id","default_front_physical_address"})||
           !c::json_uint(item.get("tentacle_id"),id,0,amity::last_tentacle_id)||(seen&(1u<<id))||
           !c::json_string(item.get("default_front_physical_address"),physical,0,7)||!device_settings::parse_front_physical_address(physical,parsed))return false;
        seen|=uint8_t(1u<<id);request.changes.push_back({{uint8_t(id),parsed}});
    }
    std::sort(request.changes.begin(),request.changes.end(),[](const Change& a,const Change& b){return a.preference.tentacle_id<b.preference.tentacle_id;});
    request.fingerprint=request.boot.str()+":"+std::to_string(request.revision);
    for(const auto& change:request.changes)request.fingerprint+=":"+std::to_string(change.preference.tentacle_id)+":"+std::to_string(change.preference.front_physical);
    return true;
}
}
void install(amity::SystemControl& value){std::lock_guard<std::mutex> lock(mutation);owner=&value;receipts.clear();}
std::string snapshot(){
    if(!owner)return "{}";
    const auto defaults=device_settings::adapter_defaults();
    const auto runtime=owner->adapter_runtime_snapshot();
    std::string out="{\"boot_id\":\""+owner->boot_identity().str()+"\",\"settings_revision\":\""+std::to_string(defaults.revision)+"\",\"adapters\":[";
    bool first=true;
    for(const auto& row:runtime){
        if(row.tentacle_id>=owner->supported_tentacles()||row.tentacle_id>=amity::max_tentacles)continue;
        if(!first)out+=',';
        first=false;
        out+="{\"tentacle_id\":"+std::to_string(row.tentacle_id)+",\"name\":\"HDMI adapter "+std::to_string(row.tentacle_id+1)+"\",\"configured\":"+(row.configured?"true":"false")+",\"default_front_physical_address\":\""+device_settings::format_physical_address(defaults.front_physical[row.tentacle_id])+"\",\"observed_front_physical_address\":"+address(row.observed_front_physical)+",\"effective_front_physical_address\":"+address(row.effective_front_physical)+"}";
    }
    return out+"]}";
}
c::Error apply(m::MessageId id,const c::Json& body,uint64_t deadline,std::string& response){
    std::lock_guard<std::mutex> lock(mutation);
    Request request;
    if(!id||!parse(body,request)){response=result(id,c::Error::invalid_message);return c::Error::invalid_message;}
    for(const auto& receipt:receipts)if(receipt.id==id){
        if(receipt.fingerprint!=request.fingerprint){response=result(id,c::Error::transaction_conflict);return c::Error::transaction_conflict;}
        response=receipt.response;return receipt.outcome;
    }
    c::Error error=c::Error::none;
    if(!owner)error=c::Error::not_ready;
    else if(request.boot!=owner->boot_identity())error=c::Error::stale_boot;
    else if(request.revision!=device_settings::adapter_defaults().revision)error=c::Error::transaction_conflict;
    else if(!deadline||now()>=deadline)error=c::Error::expired;
    else if(!c::runtime_device_settings_authorized(id))error=c::Error::not_ready;
    else {
        std::vector<device_settings::AdapterDefaultChange> preferences;
        std::vector<amity::AdapterAddressChange> adapters;
        for(const auto& change:request.changes){preferences.push_back(change.preference);adapters.push_back({change.preference.tentacle_id,change.preference.front_physical});}
        auto failure=std::make_shared<std::atomic<c::Error>>(c::Error::none);
        const auto applied=owner->commit_adapter_settings(request.boot,std::move(adapters),deadline,
            [id,deadline,revision=request.revision,preferences=std::move(preferences),failure]{
                if(now()>=deadline){failure->store(c::Error::expired);return ESP_ERR_INVALID_STATE;}
                if(!c::runtime_device_settings_authorized(id)){failure->store(c::Error::not_ready);return ESP_ERR_INVALID_STATE;}
                if(device_settings::adapter_defaults().revision!=revision){failure->store(c::Error::transaction_conflict);return ESP_ERR_INVALID_STATE;}
                const auto saved=device_settings::save_adapter_defaults(revision,preferences);
                if(saved!=ESP_OK)failure->store(saved==ESP_ERR_INVALID_STATE?c::Error::transaction_conflict:c::Error::storage_failure);
                return saved;
            });
        error=failure->load();
        if(error==c::Error::none&&applied!=ESP_OK)error=now()>=deadline?c::Error::expired:applied==ESP_ERR_NO_MEM?c::Error::queue_full:c::Error::not_ready;
    }
    response=result(id,error);
    if(receipts.size()==8)receipts.erase(receipts.begin());
    receipts.push_back({id,std::move(request.fingerprint),response,error});
    return error;
}
}
