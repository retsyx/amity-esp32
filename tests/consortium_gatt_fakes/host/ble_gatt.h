/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// GATT of the pinned NimBLE host: service and characteristic definitions, the
// access context, client results and callbacks, with the SDK's field types and
// order. Behaviour comes from the fake host in gatt_fakes.cpp.
#include "host/ble_att.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1

#define BLE_GATT_CHR_F_READ 0x00000002
#define BLE_GATT_CHR_F_WRITE 0x00000008

#define BLE_GATT_SVC_TYPE_END 0
#define BLE_GATT_SVC_TYPE_PRIMARY 1

struct ble_gatt_error {
    uint16_t status;
    uint16_t att_handle;
};

struct ble_gatt_svc {
    uint16_t start_handle;
    uint16_t end_handle;
    ble_uuid_any_t uuid;
};

struct ble_gatt_attr {
    uint16_t handle;
    uint16_t offset;
    struct os_mbuf *om;
};

struct ble_gatt_chr {
    uint16_t def_handle;
    uint16_t val_handle;
    uint8_t properties;
    ble_uuid_any_t uuid;
};

typedef int ble_gatt_mtu_fn(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg);
typedef int ble_gatt_disc_svc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service, void *arg);
typedef int ble_gatt_attr_fn(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                             void *arg);
typedef int ble_gatt_chr_fn(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                            void *arg);

struct ble_gatt_access_ctxt;
typedef int ble_gatt_access_fn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                               void *arg);
typedef uint32_t ble_gatt_chr_flags;

struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    ble_gatt_access_fn *access_cb;
    void *arg;
    struct ble_gatt_dsc_def *descriptors;
    ble_gatt_chr_flags flags;
    uint8_t min_key_size;
    uint16_t *val_handle;
    struct ble_gatt_cpfd *cpfd;
};

struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    const struct ble_gatt_svc_def **includes;
    const struct ble_gatt_chr_def *characteristics;
};

struct ble_gatt_access_ctxt {
    uint8_t op;
    struct os_mbuf *om;
    union {
        const struct ble_gatt_chr_def *chr;
        const struct ble_gatt_dsc_def *dsc;
    };
    uint16_t offset;
};

int ble_gattc_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg);
int ble_gattc_disc_svc_by_uuid(uint16_t conn_handle, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb, void *cb_arg);
int ble_gattc_disc_chrs_by_uuid(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                                const ble_uuid_t *uuid, ble_gatt_chr_fn *cb, void *cb_arg);
int ble_gattc_read(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_attr_fn *cb, void *cb_arg);
int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle, const void *data, uint16_t data_len,
                         ble_gatt_attr_fn *cb, void *cb_arg);

#ifdef __cplusplus
}
#endif
