/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "config_store.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "production_storage.hpp"
#include <vector>
#ifdef ESP_PLATFORM
#include "nvs_budget.hpp"
#include "sdkconfig.h"
#if CONFIG_AMITY_PRODUCTION_UPDATES
#include "esp_partition.h"
#include "esp_efuse.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#endif
#endif
namespace config_store {
static esp_err_t init_direct() {
#if defined(ESP_PLATFORM) && CONFIG_AMITY_PRODUCTION_UPDATES
    // Production provisioning supplies encrypted keys and encrypted initial
    // NVS contents. Never generate replacement keys or erase unreadable data.
    if(!esp_secure_boot_enabled()||!esp_efuse_is_flash_encryption_enabled()||esp_get_flash_encryption_mode()!=ESP_FLASH_ENC_MODE_RELEASE)return ESP_ERR_INVALID_STATE;
    const auto* keys=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,"nvs_keys");
    const auto* factory=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"amity_factory");
    if(!keys||!keys->encrypted||!factory||!factory->encrypted)return ESP_ERR_INVALID_STATE;
    nvs_sec_cfg_t cfg{};auto result=nvs_flash_read_security_cfg(keys,&cfg);
    static_assert(sizeof(nvs_sec_cfg_t)==64,"NVS key derivation requires the pinned 64-byte XTS key layout");
    nvs_sec_cfg_t scoped{};
    if(result==ESP_OK)result=nvs_flash_secure_init_partition("nvs",&cfg);
    for(const auto* partition:{"amity_cfg","fctry"}){
        if(result==ESP_OK&&!derive_production_storage_key(partition,reinterpret_cast<const uint8_t*>(&cfg),reinterpret_cast<uint8_t*>(&scoped)))result=ESP_FAIL;
        if(result==ESP_OK)result=nvs_flash_secure_init_partition(partition,&scoped);
        volatile uint8_t* key=reinterpret_cast<volatile uint8_t*>(&scoped);for(size_t n=0;n<sizeof(scoped);++n)key[n]=0;
    }
    volatile uint8_t* bytes=reinterpret_cast<volatile uint8_t*>(&cfg);for(size_t n=0;n<sizeof(cfg);++n)bytes[n]=0;
    return result;
#else
    return nvs_flash_init_partition("amity_cfg");
#endif
}
static esp_err_t read_direct(const char *key, std::string &value) {
    nvs_handle_t handle;
    esp_err_t e = nvs_open_from_partition("amity_cfg", "amity", NVS_READONLY, &handle);
    if (e != ESP_OK) return e;
    size_t size = 0;
    e = nvs_get_blob(handle, key, nullptr, &size);
    if (e == ESP_OK && size <= 65536) {
        std::vector<char> data(size);
        e = nvs_get_blob(handle, key, data.data(), &size);
        if(e==ESP_OK) {if(size)value.assign(data.data(),size);else value.clear();}
    } else if (e == ESP_OK) e = ESP_ERR_INVALID_SIZE;
    nvs_close(handle);
    return e;
}
static esp_err_t write_direct(const char *key, const std::string &value) {
    if (value.size() > 32768) return ESP_ERR_INVALID_SIZE;
#ifdef ESP_PLATFORM
    if (!detail::blob_space(value.size())) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
#endif
    nvs_handle_t handle;
    esp_err_t e = nvs_open_from_partition("amity_cfg", "amity", NVS_READWRITE, &handle);
    if (e != ESP_OK) return e;
    // NVS strings are limited to one page (~4 KiB); blobs span pages and keep
    // the full supported configuration size atomic across power loss.
    e = nvs_set_blob(handle, key, value.data(), value.size());
    if (e == ESP_OK) e = nvs_commit(handle);
    nvs_close(handle);
    return e;
}
esp_err_t init() {
#ifdef ESP_PLATFORM
    esp_err_t result=ESP_FAIL;return with_internal_flash_stack([&]{result=init_direct();})?result:ESP_FAIL;
#else
    return init_direct();
#endif
}
esp_err_t read(const char* key,std::string& value) {
#ifdef ESP_PLATFORM
    esp_err_t result=ESP_FAIL;return with_internal_flash_stack([&]{result=read_direct(key,value);})?result:ESP_FAIL;
#else
    return read_direct(key,value);
#endif
}
esp_err_t write(const char* key,const std::string& value) {
#ifdef ESP_PLATFORM
    esp_err_t result=ESP_FAIL;return with_internal_flash_stack([&]{result=write_direct(key,value);})?result:ESP_FAIL;
#else
    return write_direct(key,value);
#endif
}
}
