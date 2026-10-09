/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMITY_MATTER_MAX_INPUTS 16
#define AMITY_MATTER_INPUT_NAME_LENGTH 64
#define AMITY_MATTER_MAX_FABRICS 16

typedef struct {
    uint8_t index;
    char name[AMITY_MATTER_INPUT_NAME_LENGTH];
} amity_matter_input_t;

/* Called on the Matter task. Callbacks must enqueue work, not wait for CEC. */
typedef struct {
    esp_err_t (*power)(bool on, void *context);
    esp_err_t (*activity)(uint8_t index, void *context);
    esp_err_t (*key)(uint8_t cec_key, void *context);
    esp_err_t (*mute)(bool muted, void *context);
    esp_err_t (*volume_step)(int16_t steps, void *context);
    void *context;
} amity_matter_callbacks_t;

typedef struct {
    bool power;
    uint8_t current_input;
    bool mute_known;
    bool muted;
    bool volume_known;
    uint8_t volume; /* Matter 0..254; set volume_known only for observed audio status. */
} amity_matter_state_t;

typedef struct {
    uint8_t index;
    uint16_t vendor_id;
    uint64_t fabric_id;
    char label[33];
} amity_matter_fabric_t;

typedef struct {
    bool started;
    bool commissioning_window;
    uint8_t fabric_count;
    amity_matter_fabric_t fabrics[AMITY_MATTER_MAX_FABRICS];
    char manual_code[24];
    char qr_payload[128];
} amity_matter_status_t;

/* NVS must be initialized first. Matter owns WiFi and the shared NimBLE host. */
esp_err_t amity_matter_start(const amity_matter_callbacks_t *callbacks);
esp_err_t amity_matter_set_inputs(const amity_matter_input_t *inputs, size_t count);
/* Safe from an application task. Do not call while holding a lock needed by callbacks. */
esp_err_t amity_matter_update_state(const amity_matter_state_t *state);
// Unknown readings use -1. Unpaired defaults to 100%/not charging;
// paired but disconnected is unknown, regardless of supplied readings.
esp_err_t amity_matter_update_remote(bool paired, bool connected, int battery_percent, int charging);
esp_err_t amity_matter_get_status(amity_matter_status_t *status);
esp_err_t amity_matter_open_commissioning(uint16_t seconds);
esp_err_t amity_matter_close_commissioning(void);
/* Removes a fabric; removing the last Apple Home also cleans up Apple Keychain.
 * WiFi, BLE bonds, activities and configuration remain. */
esp_err_t amity_matter_remove_fabric(uint8_t fabric_index);

#ifdef __cplusplus
}
#endif
