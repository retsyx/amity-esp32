/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The station configuration read, a strict seam. Only the configuration fields
// Amity reads are present; their sizes are checked against the SDK by
// sdk_parity/network_commissioning.def.
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_IF_STA,
    WIFI_IF_AP,
} wifi_interface_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
} wifi_sta_config_t;

typedef union {
    wifi_sta_config_t sta;
} wifi_config_t;

esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf);

#ifdef __cplusplus
}
#endif
