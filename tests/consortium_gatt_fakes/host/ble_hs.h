/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The pinned NimBLE host's umbrella header: the host return codes, plus the
// headers it brings in. Values are checked by sdk_parity/ble.def.
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"
#include "host/ble_l2cap.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"

#define BLE_HS_FOREVER INT32_MAX
#define BLE_HS_CONN_HANDLE_NONE 0xffff

#define BLE_HS_EAGAIN 1
#define BLE_HS_EALREADY 2
#define BLE_HS_EINVAL 3
#define BLE_HS_EMSGSIZE 4
#define BLE_HS_ENOENT 5
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ENOTSUP 8
#define BLE_HS_EAPP 9
#define BLE_HS_EBADDATA 10
#define BLE_HS_EOS 11
#define BLE_HS_ECONTROLLER 12
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_EDONE 14
#define BLE_HS_EBUSY 15
#define BLE_HS_ERR_ATT_BASE 0x100
#define BLE_HS_ERR_HCI_BASE 0x200
#define BLE_HS_ATT_ERR(x) ((x) ? BLE_HS_ERR_ATT_BASE + (x) : 0)
#define BLE_HS_HCI_ERR(x) ((x) ? BLE_HS_ERR_HCI_BASE + (x) : 0)
