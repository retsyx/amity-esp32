/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the fake NimBLE host and CHIP BLE manager of one board.
//
// The host keeps the board's GATT database (the services ConfigureExtraServices
// registered), its advertisement, scan, connection attempt and links. A link a
// central opens to the board's own address is a loopback: the board's GATT
// client then reaches its own GATT server, as one board admitting another
// would. Any other peer runs the same firmware, so its database is the board's.
//
// Like NimBLE, the host never calls back from inside a call. It schedules each
// event and completion on the scripted clock, so callbacks run inside the
// firmware's FreeRTOS waits, or when a scenario advances the clock, on that
// thread. Scenarios are single-threaded. Every call is recorded by name.
#include "host/ble_hs.h"
#include "platform/internal/BLEManager.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace amity_fake::ble {

// Runs action once the scripted clock has advanced delay_ms from now. Actions
// due at the same time run in the order they were scheduled.
void after(int64_t delay_ms, std::function<void()> action);

// Host and BLE-manager calls so far, by function name, in call order.
std::vector<std::string> calls();
std::size_t count(std::string_view name);

// The next call of the named function returns rc and does nothing else.
void fail_next(std::string_view name, int rc);
// The next GATT client procedure of the named function is accepted and never
// completes, as when the peer stops answering.
void withhold_next(std::string_view name);
// The next discovery procedure of the named function finds nothing, as on a
// peer whose database lacks the attribute.
void empty_next(std::string_view name);

// The GATT database, with handles assigned in registration order. The BLE
// manager places the CHIPoBLE service first unless afterMatterSvc is set,
// which, as in CHIP, puts the extra services first.
struct Characteristic {
    const ble_gatt_chr_def *definition;
    uint16_t definition_handle, value_handle;
};
struct Service {
    const ble_gatt_svc_def *definition;
    uint16_t start_handle, end_handle;
    std::vector<Characteristic> characteristics;
};
std::vector<Service> services();

struct Advertisement {
    uint8_t own_address_type;
    int32_t duration_ms;
    ble_gap_adv_params params;
    ble_gap_event_fn *callback;
    void *argument;
};
// Each advertising procedure started, and whether one is running. It stops
// when a central connects, when stopped, or when its duration ends.
std::vector<Advertisement> advertisements();
bool advertising();

// An advertising report: what the board's scanner receives.
struct Report {
    ble_addr_t address;
    std::vector<uint8_t> data;
};
// The board's own address.
ble_addr_t own_address();
ble_addr_t address(uint8_t last);
// AD structures for the general-discoverable flags (unless left out), a
// 128-bit service UUID list (when given) and a complete name.
std::vector<uint8_t> advertising_payload(const std::array<uint8_t, 16> *service, std::string_view name,
                                         bool flags = true);
// The board's current advertisement, as a peer's scanner would report it.
Report own_report();

struct Scan {
    uint8_t own_address_type;
    int32_t duration_ms;
    ble_gap_disc_params params;
    ble_gap_event_fn *callback;
    void *argument;
};
// Each discovery procedure started, and whether one is running. A running
// scan reports DISC_COMPLETE when its duration ends; a cancelled one does not.
std::vector<Scan> scans();
bool scanning();
// Delivers a report to the running scan; without one, nothing is received.
void discover(const Report &report);
// Delivers an event to a scan's callback, running or not, as a late event would.
int deliver(const Scan &scan, ble_gap_event &event);
// The next scan window to end does so without DISC_COMPLETE.
void drop_scan_completion();
// Runs inside the next ble_gap_disc_cancel, before it takes effect.
void on_disc_cancel(std::function<void()> action);

struct Connect {
    uint8_t own_address_type;
    ble_addr_t peer;
    int32_t duration_ms;
    ble_gap_conn_params params;
    ble_gap_event_fn *callback;
    void *argument;
};
std::vector<Connect> connects();
// How the next connection attempt ends: delay_ms after it starts, with this
// status, link handle and negotiated parameters. By default it succeeds at
// once with handle 31 and the attempt's maximum interval and latency. A delay
// past the attempt's duration ends it with BLE_HS_ETIMEOUT then; a negative
// delay never ends it. connected runs in the same turn, after CONNECT.
struct Outcome {
    int64_t delay_ms = 0;
    int status = 0;
    uint16_t handle = 31;
    int interval = -1, latency = -1;  // -1: from the connection parameters
    std::function<void(uint16_t)> connected;
};
void connect_outcome(Outcome outcome);
// The handle the board's peripheral side gets for a loopback link.
inline constexpr uint16_t loopback_handle = 41;

// Links. A link exists from its CONNECT until its DISCONNECT has been delivered.
struct Link {
    uint16_t handle, peer;  // peer: the other end of a loopback, or BLE_HS_CONN_HANDLE_NONE
    bool central;
    uint16_t mtu, interval, latency, timeout;
    bool mtu_requested, terminating;
    ble_gap_event_fn *callback;
    void *argument;
};
bool linked(uint16_t handle);
Link link(uint16_t handle);
// A central connects to the board's advertisement: CONNECT to its callback
// with this status, and on success a link; advertising stops either way.
void accept(uint16_t handle, int status = 0);
// The link ends from the peer's side: DISCONNECT to the link's callback.
void disconnect(uint16_t handle, int reason = BLE_HS_ERR_HCI_BASE + BLE_ERR_REM_USER_CONN_TERM);
// Delivers a GAP event to a link's callback.
int link_event(uint16_t handle, ble_gap_event &event);
// Delivers BLE_GAP_EVENT_MTU for a link to its callback, with these fields.
int mtu_event(uint16_t handle, uint16_t conn_handle, uint16_t channel, uint16_t value, void *argument);
// Sets a link's ATT MTU, and its loopback peer's, as an exchange does.
void set_mtu(uint16_t handle, uint16_t mtu);
struct Termination {
    uint16_t handle;
    uint8_t reason;
};
std::vector<Termination> terminations();

// The MTU an exchange settles on, by default 527: both boards prefer it.
void preferred_mtu(uint16_t mtu);
// Replaces the host's MTU exchange; default_exchange_mtu stays available.
using ExchangeMtu = std::function<int(uint16_t, ble_gatt_mtu_fn *, void *)>;
void on_exchange_mtu(ExchangeMtu procedure);
int default_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg);

// How long each GATT client procedure takes to complete, by default 0 ms;
// the next procedures can take the given times first, one each.
void att_delay(int64_t milliseconds);
void next_att_delays(std::vector<int64_t> milliseconds);
// Each write's length as the client submitted it, before the host truncates
// it to the ATT MTU minus three, as NimBLE does.
std::vector<std::size_t> written_lengths();
// Each read response's value as the server built it, before the host
// truncates it to the ATT MTU minus one, as NimBLE does.
std::vector<std::size_t> read_lengths();
// Edits each read value the client receives, as a faulty peer would.
void on_read_value(std::function<void(std::string &)> edit);

// The board's GATT server: what a peer's write or read does there. The
// accesses of loopback procedures run these hooks first and last.
struct Access {
    uint16_t conn_handle;
    uint8_t op;
    std::string value;  // a write's value, or a read's response
    int rc;             // the access callback's result, after it ran
};
void before_server_access(std::function<void(const Access &)> hook);
void after_server_access(std::function<void(const Access &)> hook);
// A peer writes or reads the registered characteristic now, on this link.
int server_write(uint16_t conn_handle, std::string_view value);
int server_read(uint16_t conn_handle, std::string &value);

}  // namespace amity_fake::ble
