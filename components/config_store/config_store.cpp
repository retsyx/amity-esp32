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
#include <vector>
namespace config_store {
esp_err_t init() { return nvs_flash_init_partition("amity_cfg"); }
esp_err_t read(const char *key, std::string &value) {
    nvs_handle_t handle;
    esp_err_t e = nvs_open_from_partition("amity_cfg", "amity", NVS_READONLY, &handle);
    if (e != ESP_OK) return e;
    size_t size = 0;
    bool legacy_string=false;
    e = nvs_get_blob(handle, key, nullptr, &size);
    // Blob lookup first searches its blob-index item, so an existing legacy
    // string can report NOT_FOUND as well as TYPE_MISMATCH.
    if(e==ESP_ERR_NVS_TYPE_MISMATCH||e==ESP_ERR_NVS_NOT_FOUND) {
        legacy_string=true;e=nvs_get_str(handle,key,nullptr,&size);
    }
    if (e == ESP_OK && size <= 65536) {
        std::vector<char> data(size);
        e = legacy_string?nvs_get_str(handle,key,data.data(),&size):nvs_get_blob(handle,key,data.data(),&size);
        if(e==ESP_OK) {if(size)value.assign(data.data(),legacy_string?size-1:size);else value.clear();}
    } else if (e == ESP_OK) e = ESP_ERR_INVALID_SIZE;
    nvs_close(handle);
    return e;
}
esp_err_t write(const char *key, const std::string &value) {
    if (value.size() > 32768) return ESP_ERR_INVALID_SIZE;
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
}
