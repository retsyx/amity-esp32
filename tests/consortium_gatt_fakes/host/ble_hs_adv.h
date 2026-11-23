/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Advertising data of the pinned NimBLE host. The fields struct keeps the SDK's
// leading members, in order, and stops after the name; ble_hs_adv_parse_fields
// parses flags, 128-bit service UUIDs and names, and skips other AD types.
#include "host/ble_uuid.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HS_ADV_MAX_SZ 31
#define BLE_HS_ADV_MAX_FIELD_SZ (BLE_HS_ADV_MAX_SZ - 2)

#define BLE_HS_ADV_TYPE_FLAGS 0x01
#define BLE_HS_ADV_TYPE_INCOMP_UUIDS128 0x06
#define BLE_HS_ADV_TYPE_COMP_UUIDS128 0x07
#define BLE_HS_ADV_TYPE_INCOMP_NAME 0x08
#define BLE_HS_ADV_TYPE_COMP_NAME 0x09

#define BLE_HS_ADV_F_DISC_GEN 0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04

struct ble_hs_adv_fields {
    uint8_t flags;
    const ble_uuid16_t *uuids16;
    uint8_t num_uuids16;
    unsigned uuids16_is_complete : 1;
    const ble_uuid32_t *uuids32;
    uint8_t num_uuids32;
    unsigned uuids32_is_complete : 1;
    const ble_uuid128_t *uuids128;
    uint8_t num_uuids128;
    unsigned uuids128_is_complete : 1;
    const uint8_t *name;
    uint8_t name_len;
    unsigned name_is_complete : 1;
};

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *adv_fields, const uint8_t *src, uint8_t src_len);

#ifdef __cplusplus
}
#endif
