/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

#define BLE_HS_FOREVER INT32_MAX
#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ENOTSUP 8
#define BLE_HS_EAPP 9
#define BLE_HS_EBADDATA 10
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_EDONE 14
#define BLE_HS_EBUSY 15
#define BLE_HS_ENOENT 16
#define BLE_HS_EAUTHEN 23
#define BLE_HS_EPREEMPTED 29
#define BLE_HS_HCI_ERR(code) (0x200 + (code))
#define BLE_ADDR_PUBLIC 0
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_ERR_AUTH_FAIL 5
#define BLE_ERR_REM_USER_CONN_TERM 19
#define BLE_ERR_UNK_CONN_ID 2
#define BLE_ERR_CONN_SPVN_TMO 8
#define BLE_ERR_CONN_TERM_LOCAL 22
#define BLE_GAP_INITIAL_CONN_ITVL_MIN 24
#define BLE_GAP_INITIAL_CONN_ITVL_MAX 40
#define BLE_GAP_INITIAL_CONN_LATENCY 0
#define BLE_GAP_INITIAL_SUPERVISION_TIMEOUT 0x0100
#define BLE_GAP_INITIAL_CONN_MIN_CE_LEN 0
#define BLE_GAP_INITIAL_CONN_MAX_CE_LEN 0
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define BLE_GATT_CHR_PROP_READ 2
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 4
#define BLE_GATT_CHR_PROP_WRITE 8
#define BLE_GATT_CHR_PROP_NOTIFY 16
#define BLE_GATT_CHR_PROP_INDICATE 32
#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_DISC 2
#define BLE_GAP_EVENT_DISC_COMPLETE 3
#define BLE_GAP_EVENT_ENC_CHANGE 4
#define BLE_GAP_EVENT_NOTIFY_RX 5
#define BLE_GAP_EVENT_PASSKEY_ACTION 6
#define BLE_GAP_EVENT_REPEAT_PAIRING 7
#define BLE_GAP_REPEAT_PAIRING_IGNORE 0
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND 0
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND 1

typedef struct { uint8_t type, val[6]; } ble_addr_t;
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; } ble_uuid_any_t;
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_error { uint16_t status, att_handle; };
struct os_mbuf { const uint8_t *data; uint16_t length; };
struct ble_gatt_attr { uint16_t handle, offset; struct os_mbuf *om; };
#define OS_MBUF_PKTLEN(om) ((om)->length)
struct ble_gap_conn_desc {
    uint16_t conn_handle;
    ble_addr_t peer_id_addr;
    struct { unsigned encrypted : 1; unsigned bonded : 1; } sec_state;
};
struct ble_gap_conn_params {
    uint16_t scan_itvl, scan_window, itvl_min, itvl_max, latency,
        supervision_timeout, min_ce_len, max_ce_len;
};
struct ble_gap_disc_params { uint16_t itvl, window; uint8_t filter_duplicates; };
struct ble_gap_event {
    uint8_t type;
    struct { int status; uint16_t conn_handle; } connect;
    struct { int status; uint16_t conn_handle; } enc_change;
    struct { int reason; struct ble_gap_conn_desc conn; } disconnect;
    struct { uint16_t conn_handle, attr_handle; struct os_mbuf *om; } notify_rx;
    struct { uint16_t conn_handle; } passkey;
    struct {
        ble_addr_t addr;
        uint8_t event_type;
        int8_t rssi;
        const uint8_t *data;
        uint8_t length_data;
    } disc;
};
struct ble_hs_adv_fields {
    uint8_t appearance_is_present;
    uint16_t appearance;
    const ble_uuid16_t *uuids16;
    uint8_t num_uuids16;
    const uint8_t *mfg_data;
    uint8_t mfg_data_len;
    const uint8_t *name;
    uint8_t name_len;
};
struct ble_hs_cfg {
    uint8_t sm_io_cap, sm_mitm, sm_bonding, sm_sc, sm_our_key_dist, sm_their_key_dist;
};
extern struct ble_hs_cfg ble_hs_cfg;
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
typedef int ble_gatt_disc_svc_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_svc *, void *);
typedef int ble_gatt_chr_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_chr *, void *);
typedef int ble_gatt_dsc_fn(uint16_t, const struct ble_gatt_error *, uint16_t, const struct ble_gatt_dsc *, void *);
typedef int ble_gatt_attr_fn(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *);
typedef int ble_gatt_mtu_fn(uint16_t, const struct ble_gatt_error *, uint16_t, void *);
int ble_hs_synced(void);
int ble_hs_id_copy_addr(uint8_t, uint8_t *, int *);
uint16_t ble_uuid_u16(const ble_uuid_t *);
int os_mbuf_copydata(const struct os_mbuf *, int, int, void *);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *, const uint8_t *, uint8_t);
int ble_gap_connect(uint8_t, const ble_addr_t *, int32_t, const struct ble_gap_conn_params *, ble_gap_event_fn *, void *);
int ble_gap_conn_active(void);
int ble_gap_conn_cancel(void);
int ble_gap_conn_find(uint16_t, struct ble_gap_conn_desc *);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_disc(uint8_t, int32_t, const struct ble_gap_disc_params *, ble_gap_event_fn *, void *);
int ble_gap_disc_cancel(void);
int ble_gap_security_initiate(uint16_t);
int ble_store_util_delete_peer(const ble_addr_t *);
int ble_att_set_preferred_mtu(uint16_t);
int ble_gattc_disc_all_svcs(uint16_t, ble_gatt_disc_svc_fn *, void *);
int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn *, void *);
int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn *, void *);
int ble_gattc_read_long(uint16_t, uint16_t, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_flat(uint16_t, uint16_t, const void *, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_no_rsp_flat(uint16_t, uint16_t, const void *, uint16_t);
int ble_gattc_exchange_mtu(uint16_t, ble_gatt_mtu_fn *, void *);
