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
std::string current_hostname = "amity";
uint16_t current_front_physical = default_front_physical_address;
std::mutex defaults_mutex;
AdapterDefaults saved_defaults, boot_defaults;
bool parse_defaults(const std::string& bytes, AdapterDefaults& result) {
    constexpr std::string_view prefix="AMITYPA1\n";
    if(bytes.compare(0,prefix.size(),prefix)!=0)return false;
    size_t at=prefix.size(),end=bytes.find('\n',at);
    if(end==std::string::npos||end==at||end-at>20||bytes[at]=='0')return false;
    uint64_t revision=0;
    for(size_t n=at;n<end;++n){const auto b=bytes[n];if(b<'0'||b>'9'||revision>(std::numeric_limits<uint64_t>::max()-uint64_t(b-'0'))/10)return false;revision=revision*10+uint64_t(b-'0');}
    result.revision=revision;at=end+1;
    // The earlier record shape had five addresses. Validate every stored
    // address while retaining only the currently supported hardware slots.
    constexpr size_t legacy_address_count=5;
    size_t count=0;
    while(at<bytes.size()) {
        if(count>=result.front_physical.size()&&count>=legacy_address_count)return false;
        uint16_t address=0;end=bytes.find('\n',at);
        if(end==std::string::npos||end-at!=7||!parse_front_physical_address(std::string_view(bytes).substr(at,7),address))return false;
        if(count<result.front_physical.size())result.front_physical[count]=address;
        ++count;at=end+1;
    }
    return count==result.front_physical.size()||count==legacy_address_count;
}
std::string serialize_defaults(const AdapterDefaults& value){
    std::string bytes="AMITYPA1\n"+std::to_string(value.revision)+"\n";
    for(auto address:value.front_physical)bytes+=format_physical_address(address)+'\n';
    return bytes;
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
    std::lock_guard<std::mutex> lock(defaults_mutex);
    has_factory = false;
    current_hostname = "amity";
    current_front_physical = default_front_physical_address;
    saved_defaults=AdapterDefaults{};boot_defaults=AdapterDefaults{};
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
        if (has_factory) current_hostname = provisioned.hostname;
    }
    std::string override;
    auto err = config_store::read("hostname", override);
    if (err == ESP_OK) {
        if (!normalize_hostname(override)) return ESP_ERR_INVALID_STATE;
        current_hostname = override;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    err = config_store::read("front_defaults", override);
    if (err == ESP_OK) {
        if(!parse_defaults(override,saved_defaults))return ESP_ERR_INVALID_STATE;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    else {
        // The old scalar belongs to hardware slot zero only. It must never be
        // copied to newly supported or subsequently attached slots.
        err=config_store::read("front_physical",override);
        if(err==ESP_OK){if(!parse_front_physical_address(override,saved_defaults.front_physical[0]))return ESP_ERR_INVALID_STATE;}
        else if(err!=ESP_ERR_NVS_NOT_FOUND)return err;
    }
    boot_defaults=saved_defaults;current_front_physical=boot_defaults.front_physical[0];
    return ESP_OK;
}
const amity::FactoryCredentials *factory() { return has_factory ? &provisioned : nullptr; }
uint16_t front_physical_address() { return current_front_physical; }
esp_err_t save_front_physical_address(const std::string &value) {
    uint16_t address;
    if (!parse_front_physical_address(value, address)) return ESP_ERR_INVALID_ARG;
    const auto current=adapter_defaults();
    return save_adapter_defaults(current.revision,{{0,address}});
}
AdapterDefaults adapter_defaults(){std::lock_guard<std::mutex> lock(defaults_mutex);return saved_defaults;}
AdapterDefaults startup_adapter_defaults(){std::lock_guard<std::mutex> lock(defaults_mutex);return boot_defaults;}
esp_err_t save_adapter_defaults(uint64_t expected_revision,const std::vector<AdapterDefaultChange>& changes){
    std::lock_guard<std::mutex> lock(defaults_mutex);
    if(expected_revision!=saved_defaults.revision||saved_defaults.revision==std::numeric_limits<uint64_t>::max())return ESP_ERR_INVALID_STATE;
    if(changes.empty()||changes.size()>amity::max_tentacles)return ESP_ERR_INVALID_ARG;
    uint8_t changed=0;auto next=saved_defaults;
    for(const auto& change:changes){
        uint16_t parsed=0;
        if(change.tentacle_id>=amity::max_tentacles||(changed&(1u<<change.tentacle_id))||!parse_front_physical_address(format_physical_address(change.front_physical),parsed))return ESP_ERR_INVALID_ARG;
        changed|=uint8_t(1u<<change.tentacle_id);next.front_physical[change.tentacle_id]=parsed;
    }
    ++next.revision;
    const auto result=config_store::write("front_defaults",serialize_defaults(next));
    if(result==ESP_OK)saved_defaults=next;
    return result;
}
const char *hostname() { return current_hostname.c_str(); }
const char *initial_admin_auth() { return has_factory ? provisioned.admin_auth : ""; }
esp_err_t save_hostname(std::string value) {
    if (!normalize_hostname(value)) return ESP_ERR_INVALID_ARG;
    return config_store::write("hostname", value);
}
esp_err_t apply_enrollment_hostname(std::string value) {
    if (!normalize_hostname(value)) return ESP_ERR_INVALID_ARG;
    const auto result=config_store::write("hostname",value);
    if(result==ESP_OK)current_hostname=std::move(value);
    return result;
}
}
