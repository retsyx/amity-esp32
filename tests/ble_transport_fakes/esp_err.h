/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <stdint.h>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NOT_FOUND = 0x105, ESP_ERR_TIMEOUT = 0x107,
    ESP_ERR_NVS_NOT_FOUND = 0x1102;
#define CONFIG_BT_NIMBLE_ENABLED 1
#define CONFIG_BT_NIMBLE_ROLE_CENTRAL 1
#define CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI (-60)
