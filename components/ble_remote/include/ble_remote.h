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

typedef void (*ble_remote_key_callback_t)(uint16_t usage_page, uint16_t usage,
                                        bool pressed, void *context);
typedef void (*ble_remote_disconnect_callback_t)(void *context);
typedef enum { BLE_REMOTE_SIRI_BUTTONS, BLE_REMOTE_SIRI_TOUCH } ble_remote_siri_event_t;
// Siri reports use Apple's protocol, independently of the Atomic HID decoder.
// Data is valid only during this callback; copy it before returning.
typedef void (*ble_remote_siri_callback_t)(ble_remote_siri_event_t event,
                                         const uint8_t *data, size_t size, void *context);
typedef struct {
    // Address bytes use the conventional printed order, most significant first.
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    char name[48];
} ble_remote_peer_t;

typedef struct {
    bool ready;
    bool scanning;
    bool pairing;       // Automatic search/bonding operation is in progress.
    bool unpairing;     // Bond removal or link termination is still in progress.
    bool too_far;       // Weak supported-remote advertisements seen during pairing search.
    bool waiting;       // Listening in the background for the saved remote to advertise.
    bool connecting;
    bool connected;
    bool bonded;
    int charging;        // -1 unknown, 0 not charging, 1 charging.
    int battery_percent; // -1 means unknown.
    int last_error;      // ESP-IDF or NimBLE error, zero after a successful connection.
    ble_remote_peer_t peer;
} ble_remote_status_t;

// Start after the application has initialized NimBLE (Matter does this). This
// component never starts or stops the shared controller/host. Callbacks run on
// the NimBLE host task and must be short, enqueueing work rather than blocking.
esp_err_t ble_remote_start(ble_remote_key_callback_t callback, void *context);
// Register during application startup. Invoked BEFORE synthetic key releases
// on link loss/unpair, so consumers can cancel pending power/activity gestures.
void ble_remote_set_disconnect_callback(ble_remote_disconnect_callback_t callback, void *context);
void ble_remote_set_siri_callback(ble_remote_siri_callback_t callback, void *context);
esp_err_t ble_remote_scan(uint32_t seconds); // 1..60 seconds, supported remotes only.
// Search for a nearby HID or Gen 2/3 Siri Remote accepting a bond; skip rejected peers.
esp_err_t ble_remote_start_pairing(uint32_t seconds);
size_t ble_remote_scan_results(ble_remote_peer_t *peers, size_t capacity);
esp_err_t ble_remote_pair(const uint8_t address[6], uint8_t address_type);
// Unpair only the HID remote; never clears Matter fabrics or other BLE bonds.
esp_err_t ble_remote_unpair(void);
void ble_remote_get_status(ble_remote_status_t *status);

#ifdef __cplusplus
}
#endif
