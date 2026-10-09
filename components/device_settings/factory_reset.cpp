/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "device_settings.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include <atomic>
#include <vector>
namespace device_settings {
namespace {
std::atomic<bool> pending{false};
std::atomic_flag scheduling = ATOMIC_FLAG_INIT;
// The reset journal lives outside normal NVS and outside the application namespace.
// Its saved legacy credential blob survives every destructive step until completion.
struct Handle {
    nvs_handle_t value = 0;
    ~Handle() { if (value) nvs_close(value); }
};
esp_err_t read_blob(nvs_handle_t handle, const char *key, std::vector<uint8_t> &out) {
    size_t size = 0;
    auto err = nvs_get_blob(handle, key, nullptr, &size);
    if (err != ESP_OK) return err;
    if (!size || size > 1024) return ESP_ERR_INVALID_SIZE;
    out.resize(size);
    return nvs_get_blob(handle, key, out.data(), &size);
}
}
bool reset_pending() { return pending; }
esp_err_t schedule_factory_reset() {
    // The physical button and HTTPS can request a reset concurrently.
    if (scheduling.test_and_set()) return ESP_ERR_INVALID_STATE;
    struct Release {
        ~Release() { scheduling.clear(); }
    } release;
    if (pending) return ESP_ERR_INVALID_STATE;
    Handle journal;
    auto err = nvs_open_from_partition("amity_cfg", "amity_reset", NVS_READWRITE, &journal.value);
    if (err == ESP_OK) err = nvs_set_u8(journal.value, "stage", 1);
    if (err == ESP_OK) err = nvs_commit(journal.value);
    if (err == ESP_OK) pending = true;
    return err;
}
esp_err_t finish_factory_reset() {
    Handle journal;
    auto err = nvs_open_from_partition("amity_cfg", "amity_reset", NVS_READONLY, &journal.value);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t stage = 0;
    err = nvs_get_u8(journal.value, "stage", &stage);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (stage != 1 && stage != 2) return ESP_ERR_INVALID_STATE;
    nvs_close(journal.value);journal.value = 0;
    err = nvs_open_from_partition("amity_cfg", "amity_reset", NVS_READWRITE, &journal.value);
    if (err != ESP_OK) return err;
    if (stage == 1) {
        err = nvs_flash_init();
        if (err != ESP_OK) return err;
        std::vector<uint8_t> legacy;
        Handle source;
        err = nvs_open("amity_matter", NVS_READONLY, &source.value);
        if (err == ESP_OK) err = read_blob(source.value, "commissioning", legacy);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_erase_key(journal.value, "legacy");
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        } else if (err == ESP_OK) {
            err = nvs_set_blob(journal.value, "legacy", legacy.data(), legacy.size());
        }
        if (err != ESP_OK) return err;
        err = nvs_set_u8(journal.value, "stage", 2);
        if (err == ESP_OK) err = nvs_commit(journal.value);
        if (err != ESP_OK) return err;
    }
    // No network/CHIP/BLE tasks run yet, so they cannot repopulate erased storage.
    err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) return err;
    err = nvs_flash_erase();
    if (err != ESP_OK) return err;
    err = nvs_flash_init();
    if (err != ESP_OK) return err;
    std::vector<uint8_t> legacy;
    err = read_blob(journal.value, "legacy", legacy);
    if (err == ESP_OK) {
        Handle destination;
        err = nvs_open("amity_matter", NVS_READWRITE, &destination.value);
        if (err == ESP_OK) err = nvs_set_blob(destination.value, "commissioning", legacy.data(), legacy.size());
        if (err == ESP_OK) err = nvs_commit(destination.value);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK) return err;
    {
        Handle application;
        err = nvs_open_from_partition("amity_cfg", "amity", NVS_READWRITE, &application.value);
        if (err == ESP_OK) err = nvs_erase_all(application.value);
        if (err == ESP_OK) err = nvs_commit(application.value);
        if (err != ESP_OK) return err;
    }
    // Commit the cleared marker last. An interrupted reset safely repeats stage 2.
    // Keep the legacy snapshot: erasing it before the stage marker could lose
    // the original Matter code if power failed during journal cleanup.
    err = nvs_erase_key(journal.value, "stage");
    if (err == ESP_OK) err = nvs_commit(journal.value);
    if (err == ESP_OK) pending = false;
    return err;
}
}
