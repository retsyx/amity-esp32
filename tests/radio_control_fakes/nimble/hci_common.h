/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The NimBLE HCI event layouts and constants radio_control.cpp reads, checked
// against the pinned NimBLE by sdk_parity/radio_control.def.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ble_hci_ev {
    uint8_t opcode;
    uint8_t length;
    uint8_t data[0];
} __attribute__((packed));

#define BLE_HCI_OP(ogf, ocf) ((ocf) | ((ogf) << 10))
#define BLE_HCI_OGF_CTLR_BASEBAND (0x03)
#define BLE_HCI_OGF_LE (0x08)
#define BLE_HCI_OCF_CB_RESET (0x0003)
#define BLE_HCI_OCF_LE_SET_ADV_ENABLE (0x000A)

#define BLE_HCI_EVCODE_DISCONN_CMP (0x05)
struct ble_hci_ev_disconn_cmp {
    uint8_t status;
    uint16_t conn_handle;
    uint8_t reason;
} __attribute__((packed));

#define BLE_HCI_EVCODE_COMMAND_COMPLETE (0x0E)
struct ble_hci_ev_command_complete {
    uint8_t num_packets;
    uint16_t opcode;
    uint8_t status;
    uint8_t return_params[0];
} __attribute__((packed));

#define BLE_HCI_EVCODE_LE_META (0x3E)
#define BLE_HCI_LE_SUBEV_CONN_COMPLETE (0x01)
struct ble_hci_ev_le_subev_conn_complete {
    uint8_t subev_code;
    uint8_t status;
    uint16_t conn_handle;
    uint8_t role;
    uint8_t peer_addr_type;
    uint8_t peer_addr[6];
    uint16_t conn_itvl;
    uint16_t conn_latency;
    uint16_t supervision_timeout;
    uint8_t mca;
} __attribute__((packed));

#define BLE_HCI_LE_SUBEV_CONN_UPD_COMPLETE (0x03)
#define BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE (0x0A)
#define BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2 (0x29)
// The Amity build has no periodic advertising with responses, so the event
// ends at mca; the V2 form adds an advertising and a sync handle.
struct ble_hci_ev_le_subev_enh_conn_complete {
    uint8_t subev_code;
    uint8_t status;
    uint16_t conn_handle;
    uint8_t role;
    uint8_t peer_addr_type;
    uint8_t peer_addr[6];
    uint8_t local_rpa[6];
    uint8_t peer_rpa[6];
    uint16_t conn_itvl;
    uint16_t conn_latency;
    uint16_t supervision_timeout;
    uint8_t mca;
} __attribute__((packed));

#define BLE_HCI_LE_CONN_COMPLETE_ROLE_MASTER (0x00)
#define BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE (0x01)
#define BLE_HCI_LE_CONN_HANDLE_MAX (0x0eff)

#ifdef __cplusplus
}
#endif
