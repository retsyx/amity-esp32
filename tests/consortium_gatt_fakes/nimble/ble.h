/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Device addresses and HCI error codes of the pinned NimBLE, checked by
// sdk_parity/ble.def.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_ADDR_PUBLIC (0x00)
#define BLE_ADDR_RANDOM (0x01)
#define BLE_OWN_ADDR_PUBLIC (0x00)

typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;

enum ble_error_codes {
    BLE_ERR_SUCCESS = 0x00,
    BLE_ERR_REM_USER_CONN_TERM = 0x13,
    BLE_ERR_CONN_TERM_LOCAL = 0x16,
};

#ifdef __cplusplus
}
#endif
