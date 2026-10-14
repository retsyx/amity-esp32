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
namespace device_settings {
namespace {
amity::FactoryCredentials provisioned{};
bool has_factory = false;
std::string current_hostname = "amity";
uint16_t current_front_physical = default_front_physical_address;
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
    has_factory = false;
    current_hostname = "amity";
    current_front_physical = default_front_physical_address;
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
    err = config_store::read("front_physical", override);
    if (err == ESP_OK) {
        if (!parse_front_physical_address(override, current_front_physical)) return ESP_ERR_INVALID_STATE;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    return ESP_OK;
}
const amity::FactoryCredentials *factory() { return has_factory ? &provisioned : nullptr; }
uint16_t front_physical_address() { return current_front_physical; }
esp_err_t save_front_physical_address(const std::string &value) {
    uint16_t address;
    if (!parse_front_physical_address(value, address)) return ESP_ERR_INVALID_ARG;
    return config_store::write("front_physical", format_physical_address(address));
}
const char *hostname() { return current_hostname.c_str(); }
const char *initial_admin_auth() { return has_factory ? provisioned.admin_auth : ""; }
esp_err_t save_hostname(std::string value) {
    if (!normalize_hostname(value)) return ESP_ERR_INVALID_ARG;
    return config_store::write("hostname", value);
}
}
