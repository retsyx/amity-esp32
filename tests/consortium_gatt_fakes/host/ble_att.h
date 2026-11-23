/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// ATT limits and error codes of the pinned NimBLE host.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_ATT_ERR_INVALID_HANDLE 0x01
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 0x05
#define BLE_ATT_ERR_INVALID_OFFSET 0x07
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11

#define BLE_ATT_ATTR_MAX_LEN 512
#define BLE_ATT_MTU_DFLT 23

// The connection's ATT MTU: 23 until an MTU exchange completes.
uint16_t ble_att_mtu(uint16_t conn_handle);

#ifdef __cplusplus
}
#endif
