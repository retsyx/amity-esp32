/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// UUIDs of the pinned NimBLE host. ble_uuid_cmp orders by type, then value.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BLE_UUID_TYPE_16 = 16,
    BLE_UUID_TYPE_32 = 32,
    BLE_UUID_TYPE_128 = 128,
};

typedef struct {
    uint8_t type;
} ble_uuid_t;

typedef struct {
    ble_uuid_t u;
    uint16_t value;
} ble_uuid16_t;

typedef struct {
    ble_uuid_t u;
    uint32_t value;
} ble_uuid32_t;

typedef struct {
    ble_uuid_t u;
    uint8_t value[16];
} ble_uuid128_t;

typedef union {
    ble_uuid_t u;
    ble_uuid16_t u16;
    ble_uuid32_t u32;
    ble_uuid128_t u128;
} ble_uuid_any_t;

#define BLE_UUID128_INIT(uuid128...) {{BLE_UUID_TYPE_128}, {uuid128}}

int ble_uuid_cmp(const ble_uuid_t *uuid1, const ble_uuid_t *uuid2);

#ifdef __cplusplus
}
#endif
