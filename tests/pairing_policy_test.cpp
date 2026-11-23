/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "pairing_policy.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    using ble::pairing_candidate;
    assert(pairing_candidate(true, true, false, -50, -50));
    assert(pairing_candidate(true, true, false, -35, -50));
    assert(!pairing_candidate(true, true, false, -51, -50));
    assert(!pairing_candidate(false, true, false, -30, -50));
    assert(!pairing_candidate(true, false, false, -30, -50));
    assert(!pairing_candidate(true, true, true, -30, -50));
    assert(pairing_candidate(true, true, false, -70, -75));
    puts("Remote pairing RSSI boundary, HID, connectability and rejected-peer tests passed");

    using ble::PairingStage;
    using ble::pairing_link_failure;
    const auto hci = [](int reason) { return ble::hci_error_base + reason; };
    // A link lost before NimBLE reports it ends with its HCI reason.
    for (const int reason : {0x3e, 0x08, 0x13, 0x22}) assert(pairing_link_failure(PairingStage::Connecting, hci(reason)));
    // A connection attempt that made no link: timed out (13), cancelled (9) or not started (15).
    for (const int error : {0, 13, 9, 15, 0x300}) assert(!pairing_link_failure(PairingStage::Connecting, error));
    // A link lost after NimBLE reports it, before security starts, ends with its
    // disconnect reason, or not connected (7) for a routine one. Each counts,
    // even a remote's own Authentication Failure (0x05).
    for (const int error : {ble::not_connected, hci(0x05), hci(0x22), hci(0x3d)})
        assert(pairing_link_failure(PairingStage::Connected, error));
    assert(!pairing_link_failure(PairingStage::Connected, 0));
    // A link lost during pairing.
    assert(pairing_link_failure(PairingStage::Securing, ble::not_connected));
    // Refusals: a peer or local Security Manager failure, a passkey request or
    // missing encryption (23), a security timeout (13), encryption refused on
    // a live link.
    for (const int error : {0x505, 0x404, 23, 13, hci(0x06)}) assert(!pairing_link_failure(PairingStage::Securing, error));
    // After encryption every failure belongs to setup: a lost link, an
    // unsupported remote (8), an attribute error.
    for (const int error : {0, ble::not_connected, 8, 0x10e, hci(0x08)})
        assert(!pairing_link_failure(PairingStage::Secured, error));

    using ble::retry_pairing_link;
    assert(retry_pairing_link(true, 0, false));
    assert(retry_pairing_link(true, 1, false));
    assert(!retry_pairing_link(true, 2, false)); // Three attempts in all.
    assert(!retry_pairing_link(false, 0, false));
    assert(!retry_pairing_link(true, 0, true)); // A bond left behind ends the search.

    using ble::pairing_attempt_ms;
    // A first attempt takes the full time, however little of the search is left.
    for (const int64_t remaining : {20000, 10000, 300, 1}) assert(pairing_attempt_ms(0, remaining) == 10000);
    // A retry ends with the search and needs two seconds of it.
    assert(pairing_attempt_ms(1, 15000) == 10000 && pairing_attempt_ms(2, 10000) == 10000);
    assert(pairing_attempt_ms(1, 9999) == 9999 && pairing_attempt_ms(2, 2000) == 2000);
    for (const int64_t remaining : {1999, 100, 0}) assert(!pairing_attempt_ms(1, remaining));
    puts("Remote pairing link failure and retry tests passed");
}
