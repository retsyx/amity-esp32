/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "device_settings.hpp"
#include "config_store.hpp"
#include "esp_partition.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <cstring>
#include <utility>
#include <limits>
#include <mutex>
namespace device_settings {
namespace {
amity::FactoryCredentials provisioned{};
bool has_factory = false;
std::mutex defaults_mutex;
std::mutex adapter_mutation;
std::mutex hostname_mutation;
HostnameHandler hostname_handler;
AdapterDefaults saved_defaults;
HostnameSettings saved_hostname;
bool parse_revision(const std::string& bytes,size_t& at,uint64_t& revision){
    const auto end=bytes.find('\n',at);
    if(end==std::string::npos||end==at||end-at>20||bytes[at]=='0')return false;
    revision=0;
    for(size_t n=at;n<end;++n){const auto b=bytes[n];if(b<'0'||b>'9'||revision>(std::numeric_limits<uint64_t>::max()-uint64_t(b-'0'))/10)return false;revision=revision*10+uint64_t(b-'0');}
    at=end+1;return true;
}
bool parse_defaults(const std::string& bytes, AdapterDefaults& result) {
    constexpr std::string_view prefix="AMITYPA1\n";
    if(bytes.compare(0,prefix.size(),prefix)!=0)return false;
    size_t at=prefix.size();
    if(!parse_revision(bytes,at,result.revision))return false;
    size_t count=0;
    while(at<bytes.size()) {
        if(count>=result.front_physical.size())return false;
        uint16_t address=0;const auto end=bytes.find('\n',at);
        if(end==std::string::npos||end-at!=7||!parse_front_physical_address(std::string_view(bytes).substr(at,7),address))return false;
        result.front_physical[count]=address;
        ++count;at=end+1;
    }
    return count==result.front_physical.size();
}
std::string serialize_defaults(const AdapterDefaults& value){
    std::string bytes="AMITYPA1\n"+std::to_string(value.revision)+"\n";
    for(auto address:value.front_physical)bytes+=format_physical_address(address)+'\n';
    return bytes;
}
bool parse_hostname(const std::string& bytes,HostnameSettings& result){
    constexpr std::string_view prefix="AMITYHN1\n";
    if(bytes.compare(0,prefix.size(),prefix)!=0)return false;
    size_t at=prefix.size();
    if(!parse_revision(bytes,at,result.revision))return false;
    const auto end=bytes.find('\n',at);
    if(end==std::string::npos||end+1!=bytes.size())return false;
    result.hostname=bytes.substr(at,end-at);
    return normalize_hostname(result.hostname)&&result.hostname==bytes.substr(at,end-at);
}
}
bool normalize_hostname(std::string &value) {
    if (value.empty() || value.size() > 32) return false;
    for (auto &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    uint8_t bytes[64]{};
    memcpy(bytes, value.data(), value.size());
    // Embedded NULs are not accepted, even with otherwise valid padding.
    return value.find('\0') == std::string::npos && amity::valid_factory_hostname(bytes);
}
esp_err_t init() {
    std::scoped_lock mutation(adapter_mutation,hostname_mutation);
    std::lock_guard<std::mutex> lock(defaults_mutex);
    has_factory = false;
    saved_defaults=AdapterDefaults{};
    saved_hostname=HostnameSettings{};
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "amity_factory");
    if (partition) {
        uint8_t record[amity::factory_record_size], digest[32];
        auto err = esp_partition_read(partition, 0, record, sizeof(record));
        if (err != ESP_OK) return err;
        size_t length = 0;
        if (psa_crypto_init() != PSA_SUCCESS ||
            psa_hash_compute(PSA_ALG_SHA_256, record, amity::factory_record_digest_offset,
                             digest, sizeof(digest), &length) != PSA_SUCCESS || length != sizeof(digest)) return ESP_FAIL;
        auto result = amity::parse_factory_record(record, sizeof(record), digest,
            CONFIG_DEVICE_VENDOR_ID, CONFIG_DEVICE_PRODUCT_ID, provisioned);
        if (result == amity::FactoryRecord::Invalid) {
            ESP_LOGE("device_settings", "Invalid factory provisioning data");
            return ESP_ERR_INVALID_STATE;
        }
        has_factory = result == amity::FactoryRecord::Valid;
        if (has_factory) saved_hostname.hostname = provisioned.hostname;
    }
    std::string override;
    auto err = config_store::read("front_defaults", override);
    if (err == ESP_OK) {
        if(!parse_defaults(override,saved_defaults))return ESP_ERR_INVALID_STATE;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    err=config_store::read("hostname",override);
    if(err==ESP_OK){if(!parse_hostname(override,saved_hostname))return ESP_ERR_INVALID_STATE;}
    else if(err!=ESP_ERR_NVS_NOT_FOUND)return err;
    return ESP_OK;
}
const amity::FactoryCredentials *factory() { return has_factory ? &provisioned : nullptr; }
uint16_t front_physical_address() { std::lock_guard<std::mutex> lock(defaults_mutex);return saved_defaults.front_physical[0]; }
esp_err_t save_front_physical_address(const std::string &value) {
    uint16_t address;
    if (!parse_front_physical_address(value, address)) return ESP_ERR_INVALID_ARG;
    const auto current=adapter_defaults();
    return save_adapter_defaults(current.revision,{{0,address}});
}
AdapterDefaults adapter_defaults(){std::lock_guard<std::mutex> lock(defaults_mutex);return saved_defaults;}
esp_err_t save_adapter_defaults(uint64_t expected_revision,const std::vector<AdapterDefaultChange>& changes){
    std::lock_guard<std::mutex> mutation(adapter_mutation);
    const auto previous=adapter_defaults();
    if(expected_revision!=previous.revision||previous.revision==std::numeric_limits<uint64_t>::max())return ESP_ERR_INVALID_STATE;
    if(changes.empty()||changes.size()>amity::max_tentacles)return ESP_ERR_INVALID_ARG;
    uint8_t changed=0;auto next=previous;
    for(const auto& change:changes){
        uint16_t parsed=0;
        if(change.tentacle_id>=amity::max_tentacles||(changed&(1u<<change.tentacle_id))||!parse_front_physical_address(format_physical_address(change.front_physical),parsed))return ESP_ERR_INVALID_ARG;
        changed|=uint8_t(1u<<change.tentacle_id);next.front_physical[change.tentacle_id]=parsed;
    }
    ++next.revision;
    const auto result=config_store::write("front_defaults",serialize_defaults(next));
    if(result==ESP_OK){std::lock_guard<std::mutex> lock(defaults_mutex);saved_defaults=std::move(next);}
    return result;
}
void set_hostname_handler(HostnameHandler handler){std::lock_guard<std::mutex> mutation(hostname_mutation);hostname_handler=std::move(handler);}
HostnameSettings hostname_settings(){std::lock_guard<std::mutex> lock(defaults_mutex);return saved_hostname;}
esp_err_t save_hostname(uint64_t expected_revision,std::string hostname){
    std::lock_guard<std::mutex> mutation(hostname_mutation);
    const auto previous=hostname_settings();
    if(expected_revision!=previous.revision||previous.revision==std::numeric_limits<uint64_t>::max())return ESP_ERR_INVALID_STATE;
    if(!normalize_hostname(hostname))return ESP_ERR_INVALID_ARG;
    HostnameSettings next{previous.revision+1,std::move(hostname)};
    const bool renamed=next.hostname!=previous.hostname;
    if(renamed&&hostname_handler){const auto applied=hostname_handler(previous.hostname,next.hostname);if(applied!=ESP_OK)return applied;}
    const auto result=config_store::write("hostname","AMITYHN1\n"+std::to_string(next.revision)+"\n"+next.hostname+"\n");
    if(result==ESP_OK){std::lock_guard<std::mutex> lock(defaults_mutex);saved_hostname=std::move(next);}
    else if(renamed&&hostname_handler){
        const auto restored=hostname_handler(next.hostname,previous.hostname);
        if(restored!=ESP_OK)ESP_LOGE("device_settings","Hostname rollback failed: %d",int(restored));
    }
    return result;
}
const char *hostname() {
    thread_local char snapshot[33];
    std::lock_guard<std::mutex> lock(defaults_mutex);
    std::memcpy(snapshot,saved_hostname.hostname.c_str(),saved_hostname.hostname.size()+1);
    return snapshot;
}
const char *initial_admin_auth() { return has_factory ? provisioned.admin_auth : ""; }
esp_err_t apply_enrollment_hostname(std::string value) {
    return save_hostname(hostname_settings().revision,std::move(value));
}
}
