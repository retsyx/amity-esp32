/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// GAP of the pinned NimBLE host: the event numbers, parameter and descriptor
// structs, and the union members Amity reads, with the SDK's field types and
// order. Behaviour comes from the fake host in gatt_fakes.cpp.
#include "host/ble_hs_adv.h"
#include "nimble/ble.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_DISC 7
#define BLE_GAP_EVENT_DISC_COMPLETE 8
#define BLE_GAP_EVENT_ADV_COMPLETE 9
#define BLE_GAP_EVENT_MTU 15

#define BLE_GAP_CONN_MODE_NON 0
#define BLE_GAP_CONN_MODE_DIR 1
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_NON 0
#define BLE_GAP_DISC_MODE_LTD 1
#define BLE_GAP_DISC_MODE_GEN 2

struct ble_gap_sec_state {
    unsigned encrypted : 1;
    unsigned authenticated : 1;
    unsigned bonded : 1;
    unsigned key_size : 5;
    unsigned authorize : 1;
};

struct ble_gap_adv_params {
    uint8_t conn_mode;
    uint8_t disc_mode;
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint8_t channel_map;
    uint8_t filter_policy;
    uint8_t high_duty_cycle : 1;
};

struct ble_gap_conn_desc {
    struct ble_gap_sec_state sec_state;
    ble_addr_t our_id_addr;
    ble_addr_t peer_id_addr;
    ble_addr_t our_ota_addr;
    ble_addr_t peer_ota_addr;
    uint16_t conn_handle;
    uint16_t conn_itvl;
    uint16_t conn_latency;
    uint16_t supervision_timeout;
    uint8_t role;
    uint8_t master_clock_accuracy;
};

struct ble_gap_conn_params {
    uint16_t scan_itvl;
    uint16_t scan_window;
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
    uint16_t min_ce_len;
    uint16_t max_ce_len;
};

struct ble_gap_disc_params {
    uint16_t itvl;
    uint16_t window;
    uint8_t filter_policy;
    uint8_t limited : 1;
    uint8_t passive : 1;
    uint8_t filter_duplicates : 1;
    uint8_t disable_observer_mode : 1;
};

struct ble_gap_disc_desc {
    uint8_t event_type;
    uint8_t length_data;
    ble_addr_t addr;
    int8_t rssi;
    const uint8_t *data;
    ble_addr_t direct_addr;
};

struct ble_gap_event {
    uint8_t type;
    union {
        struct {
            int status;
            uint16_t conn_handle;
        } connect;
        struct {
            int reason;
            struct ble_gap_conn_desc conn;
        } disconnect;
        struct ble_gap_disc_desc disc;
        struct {
            int reason;
        } disc_complete;
        struct {
            int reason;
        } adv_complete;
        struct {
            uint16_t conn_handle;
            uint16_t channel_id;
            uint16_t value;
        } mtu;
    };
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);

int ble_gap_adv_start(uint8_t own_addr_type, const ble_addr_t *direct_addr, int32_t duration_ms,
                      const struct ble_gap_adv_params *adv_params, ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_adv_stop(void);
int ble_gap_adv_active(void);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *rsp_fields);
int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms, const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_disc_cancel(void);
int ble_gap_disc_active(void);
int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr, int32_t duration_ms,
                    const struct ble_gap_conn_params *params, ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_conn_cancel(void);
int ble_gap_conn_active(void);
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);
int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc);

#ifdef __cplusplus
}
#endif
