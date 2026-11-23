/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>

namespace ble {
inline bool pairing_candidate(bool hid, bool connectable, bool attempted,
                              int rssi, int minimum_rssi) {
    return hid && connectable && !attempted && rssi >= minimum_rssi;
}

// NimBLE host results (host/ble_hs.h) the policy reads. The remote checks
// them against NimBLE when it is compiled.
constexpr int not_connected = 7;      // BLE_HS_ENOTCONN
constexpr int hci_error_base = 0x200; // BLE_HS_ERR_HCI_BASE, plus the HCI reason

// How far a pairing attempt got before it failed: NimBLE had not reported the
// link, had reported it before security started, security had started, or the
// link was encrypted.
enum class PairingStage { Connecting, Connected, Securing, Secured };

// A link that failed before it carried security says nothing about whether
// the remote accepts a bond. NimBLE reports a link only after reading the
// remote's features and version; one lost before then ends the connection
// with its HCI reason. One lost after the report, before security started,
// ends with its disconnect reason; one lost during pairing is not connected.
// Security Manager failures, passkey requests, unsupported remotes and setup
// errors are a remote that engaged and refused.
inline bool pairing_link_failure(PairingStage stage, int error) {
    if (stage == PairingStage::Connecting)
        return error > hci_error_base && error < hci_error_base + 0x100;
    if (stage == PairingStage::Connected) return error != 0;
    return stage == PairingStage::Securing && error == not_connected;
}

// A search tries a remote again after its link failed before security, at
// most twice, but not once a bond the attempt left behind could not be removed.
constexpr unsigned pairing_link_retries = 2;
inline bool retry_pairing_link(bool link_failure, unsigned retries, bool bond_cleanup_failed) {
    return link_failure && retries < pairing_link_retries && !bond_cleanup_failed;
}

// How long a pairing connection attempt may take, in milliseconds. A remote's
// first attempt takes the full time. A retry ends with the search, and with
// less than two seconds of it left is not started (zero): a connection cut
// short would end in a timeout that replaces the link error.
constexpr int32_t pairing_connect_ms = 10000;
constexpr int32_t pairing_retry_minimum_ms = 2000;
inline int32_t pairing_attempt_ms(unsigned retries, int64_t remaining_ms) {
    if (!retries) return pairing_connect_ms;
    if (remaining_ms < pairing_retry_minimum_ms) return 0;
    return remaining_ms < pairing_connect_ms ? static_cast<int32_t>(remaining_ms) : pairing_connect_ms;
}
} // namespace ble
