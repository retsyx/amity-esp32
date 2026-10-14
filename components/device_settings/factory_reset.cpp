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
namespace device_settings {
namespace {
std::atomic<bool> pending{false};
std::atomic_flag scheduling = ATOMIC_FLAG_INIT;
// The reset journal lives outside normal NVS and outside the application namespace.
struct Handle {
    nvs_handle_t value = 0;
    ~Handle() { if (value) nvs_close(value); }
};
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
    if (err == ESP_OK) err = nvs_set_u8(journal.value, "pending", 1);
    if (err == ESP_OK) err = nvs_commit(journal.value);
    if (err == ESP_OK) pending = true;
    return err;
}
esp_err_t finish_factory_reset() {
    Handle journal;
    auto err = nvs_open_from_partition("amity_cfg", "amity_reset", NVS_READONLY, &journal.value);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t marker = 0;
    err = nvs_get_u8(journal.value, "pending", &marker);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (marker != 1) return ESP_ERR_INVALID_STATE;
    nvs_close(journal.value);journal.value = 0;
    err = nvs_open_from_partition("amity_cfg", "amity_reset", NVS_READWRITE, &journal.value);
    if (err != ESP_OK) return err;
    // No network/CHIP/BLE tasks run yet, so they cannot repopulate erased storage.
    err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) return err;
    err = nvs_flash_erase();
    if (err != ESP_OK) return err;
    err = nvs_flash_init();
    if (err != ESP_OK) return err;
    {
        Handle application;
        err = nvs_open_from_partition("amity_cfg", "amity", NVS_READWRITE, &application.value);
        if (err == ESP_OK) err = nvs_erase_all(application.value);
        if (err == ESP_OK) err = nvs_commit(application.value);
        if (err != ESP_OK) return err;
    }
    // Clear the marker last so an interrupted reset safely repeats all erases.
    // Commissioning credentials remain in the untouched factory partition.
    err = nvs_erase_key(journal.value, "pending");
    if (err == ESP_OK) err = nvs_commit(journal.value);
    if (err == ESP_OK) pending = false;
    return err;
}
}
