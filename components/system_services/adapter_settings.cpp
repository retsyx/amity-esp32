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
uint64_t now(){return static_cast<uint64_t>(esp_timer_get_time()/1000);}
std::string address(std::optional<uint16_t> value){return value&&*value!=0xffff?'"'+device_settings::format_physical_address(*value)+'"':"null";}
}
void install(amity::SystemControl& value){std::lock_guard<std::mutex> lock(mutation);owner=&value;}
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
c::Error apply(m::MessageId id,const c::DeviceSettingsWrite& write,uint64_t deadline,std::string& saved){
    std::lock_guard<std::mutex> lock(mutation);saved.clear();
    if(!id)return c::Error::invalid_message;
    std::vector<device_settings::AdapterDefaultChange> preferences;
    std::vector<amity::AdapterAddressChange> adapters;
    for(const auto& change:write.changes){
        uint16_t address=0;
        if(!device_settings::parse_front_physical_address(change.default_front_physical_address,address))return c::Error::invalid_message;
        preferences.push_back({change.tentacle_id,address});adapters.push_back({change.tentacle_id,address});
    }
    c::Error error=c::Error::none;
    if(!owner)error=c::Error::not_ready;
    else if(write.expected_boot!=owner->boot_identity())error=c::Error::stale_boot;
    else if(write.hostname?write.expected_hostname_revision!=device_settings::hostname_settings().revision:write.expected_revision!=device_settings::adapter_defaults().revision)error=c::Error::transaction_conflict;
    else if(!deadline||now()>=deadline)error=c::Error::expired;
    else if(!c::runtime_device_settings_authorized(id))error=c::Error::not_ready;
    else {
        auto failure=std::make_shared<std::atomic<c::Error>>(c::Error::none);
        auto persist=[id,deadline,revision=write.expected_revision,hostname_revision=write.expected_hostname_revision,preferences=std::move(preferences),hostname=write.hostname,failure]{
                if(now()>=deadline){failure->store(c::Error::expired);return ESP_ERR_INVALID_STATE;}
                if(!c::runtime_device_settings_authorized(id)){failure->store(c::Error::not_ready);return ESP_ERR_INVALID_STATE;}
                if(hostname?device_settings::hostname_settings().revision!=hostname_revision:device_settings::adapter_defaults().revision!=revision){failure->store(c::Error::transaction_conflict);return ESP_ERR_INVALID_STATE;}
                const auto saved=hostname?device_settings::save_hostname(hostname_revision,*hostname):device_settings::save_adapter_defaults(revision,preferences);
                if(saved==ESP_OK&&hostname)c::runtime_hostname_changed();
                if(saved!=ESP_OK)failure->store(saved==ESP_ERR_INVALID_STATE?c::Error::transaction_conflict:c::Error::storage_failure);
                return saved;
            };
        // Each field has its own record and revision. A rename changes only
        // service metadata; adapter changes retain the HDMI owner's gate.
        const auto applied=write.hostname?persist():owner->commit_adapter_settings(write.expected_boot,std::move(adapters),deadline,std::move(persist));
        error=failure->load();
        if(error==c::Error::none&&applied!=ESP_OK)error=now()>=deadline?c::Error::expired:applied==ESP_ERR_NO_MEM?c::Error::queue_full:c::Error::not_ready;
    }
    if(error==c::Error::none)saved=snapshot();
    return error;
}
}
