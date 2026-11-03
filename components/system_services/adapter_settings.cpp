// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "adapter_settings.hpp"
#include "amity/system_control.hpp"
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/settings.hpp"
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
struct Request {m::BootId boot;uint64_t revision=0,hostname_revision=0;std::vector<Change> changes;std::optional<std::string> hostname;std::string fingerprint;};
bool parse(const c::Json& json,Request& request){
    c::DeviceSettingsWrite write;
    if(c::parse_device_settings_write(json,write)!=c::Error::none)return false;
    request.boot=write.expected_boot;request.revision=write.expected_revision;request.hostname_revision=write.expected_hostname_revision;request.hostname=std::move(write.hostname);
    for(const auto& item:write.changes){
        uint16_t parsed=0;
        if(!device_settings::parse_front_physical_address(item.default_front_physical_address,parsed))return false;
        request.changes.push_back({{item.tentacle_id,parsed}});
    }
    std::sort(request.changes.begin(),request.changes.end(),[](const Change& a,const Change& b){return a.preference.tentacle_id<b.preference.tentacle_id;});
    request.fingerprint=request.boot.str()+":"+std::to_string(request.revision);
    if(request.hostname)request.fingerprint+=":hostname="+*request.hostname+":"+std::to_string(request.hostname_revision);
    for(const auto& change:request.changes)request.fingerprint+=":"+std::to_string(change.preference.tentacle_id)+":"+std::to_string(change.preference.front_physical);
    return true;
}
}
void install(amity::SystemControl& value){std::lock_guard<std::mutex> lock(mutation);owner=&value;receipts.clear();}
std::string snapshot(){
    if(!owner)return "{}";
    const auto defaults=device_settings::adapter_defaults();
    const auto hostname=device_settings::hostname_settings();
    const auto runtime=owner->adapter_runtime_snapshot();
    std::string out="{\"boot_id\":\""+owner->boot_identity().str()+"\",\"settings_revision\":\""+std::to_string(defaults.revision)+"\",\"hostname_revision\":\""+std::to_string(hostname.revision)+"\",\"hostname\":\""+hostname.hostname+"\",\"adapters\":[";
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
    else if(request.hostname?request.hostname_revision!=device_settings::hostname_settings().revision:request.revision!=device_settings::adapter_defaults().revision)error=c::Error::transaction_conflict;
    else if(!deadline||now()>=deadline)error=c::Error::expired;
    else if(!c::runtime_device_settings_authorized(id))error=c::Error::not_ready;
    else {
        std::vector<device_settings::AdapterDefaultChange> preferences;
        std::vector<amity::AdapterAddressChange> adapters;
        for(const auto& change:request.changes){preferences.push_back(change.preference);adapters.push_back({change.preference.tentacle_id,change.preference.front_physical});}
        auto failure=std::make_shared<std::atomic<c::Error>>(c::Error::none);
        auto persist=[id,deadline,revision=request.revision,hostname_revision=request.hostname_revision,preferences=std::move(preferences),hostname=request.hostname,failure]{
                if(now()>=deadline){failure->store(c::Error::expired);return ESP_ERR_INVALID_STATE;}
                if(!c::runtime_device_settings_authorized(id)){failure->store(c::Error::not_ready);return ESP_ERR_INVALID_STATE;}
                if(hostname?device_settings::hostname_settings().revision!=hostname_revision:device_settings::adapter_defaults().revision!=revision){failure->store(c::Error::transaction_conflict);return ESP_ERR_INVALID_STATE;}
                const auto saved=hostname?device_settings::save_hostname(hostname_revision,*hostname):device_settings::save_adapter_defaults(revision,preferences);
                if(saved!=ESP_OK)failure->store(saved==ESP_ERR_INVALID_STATE?c::Error::transaction_conflict:c::Error::storage_failure);
                return saved;
            };
        // Each field has its own record and revision. A rename changes only
        // service metadata; adapter changes retain the HDMI owner's gate.
        const auto applied=request.hostname?persist():owner->commit_adapter_settings(request.boot,std::move(adapters),deadline,std::move(persist));
        error=failure->load();
        if(error==c::Error::none&&applied!=ESP_OK)error=now()>=deadline?c::Error::expired:applied==ESP_ERR_NO_MEM?c::Error::queue_full:c::Error::not_ready;
    }
    response=result(id,error);
    if(receipts.size()==8)receipts.erase(receipts.begin());
    receipts.push_back({id,std::move(request.fingerprint),response,error});
    return error;
}
}
