/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The HomeKit SDK's accessory database and setup calls, as homekit.cpp uses
// them. Accessories, services and characteristics are recorded with their
// values, constraints and callbacks, so a scenario can see what HomeKit would
// publish and call the read and write callbacks HomeKit would call.
// hap_char_update_val follows the SDK: a numeric value outside the
// characteristic's constraints is refused, a string is copied, a TLV8 value
// keeps the caller's buffer, and controllers are notified when the value
// changes, or on every TLV8 update.
#include "hap.h"
#include <cstdint>
#include <string>
#include <vector>

namespace amity_fake::hap {

enum class Format { boolean, uint8, uint32, string, tlv8 };

struct Service;

struct Characteristic {
    std::string uuid;
    uint16_t permissions = 0;
    Format format = Format::uint8;
    // A string value points into text; a TLV8 value at the caller's buffer.
    hap_val_t value{};
    std::string text;
    bool constrained = false;
    int min = 0, max = 0, step = 0;
    int maxlen = 0;
    // hap_char_update_val calls, accepted updates that notified controllers,
    // and hap_char_get_val calls.
    unsigned updates = 0, notifications = 0, gets = 0;
    Service *service = nullptr;

    uint32_t number() const;
    std::vector<uint8_t> bytes() const;
};

struct Accessory;

struct Service {
    std::string uuid;
    std::vector<Characteristic *> characteristics;
    hap_serv_bulk_read_t read = nullptr;
    hap_serv_write_t write = nullptr;
    bool primary = false;
    std::vector<Service *> linked;
    Accessory *accessory = nullptr;
    // The characteristic of this type; fails the scenario unless there is exactly one.
    Characteristic &characteristic(const std::string &type) const;
};

struct Accessory {
    std::string name, model, manufacturer, serial, firmware, hardware, protocol;
    hap_cid_t category{};
    hap_identify_routine_t identify = nullptr;
    std::vector<Service *> services;
    bool added = false;
};

// Created accessories and services, in creation order.
std::vector<Accessory *> accessories();
std::vector<Service *> services();
// The index-th created service of this type; fails the scenario if there is none.
Service &service(const std::string &type, unsigned index = 0);

// Calls the characteristic's service's bulk read callback, as HAP does for
// GET /characteristics, and returns the status it reported.
hap_status_t read(const Characteristic &characteristic);
// Calls the service's write callback with one value, as HAP does for
// PUT /characteristics, and returns the status it reported.
hap_status_t write(const Characteristic &characteristic, hap_val_t value);

struct SetupCalls {
    unsigned get_config = 0, set_config = 0, init = 0, start = 0, set_setup_id = 0, set_setup_info = 0, payloads = 0;
};
SetupCalls setup_calls();
// The configuration hap_get_config reports, which hap_set_config replaces.
hap_cfg_t configuration();
hap_transport_t transport();
std::string setup_id();
hap_setup_info_t setup_info();

// The next count calls of the named setup function fail: hap_set_config,
// hap_init, hap_start, hap_set_setup_id or hap_set_setup_info return HAP_FAIL;
// the named creation function (hap_acc_create, hap_serv_create,
// hap_char_<format>_create), hap_serv_add_char or esp_hap_get_setup_payload
// fails its way.
void fail(const std::string &function, unsigned count = 1);

void set_paired_controllers(int count);

struct PayloadRequest {
    std::string code, id;
    bool wac = false;
    hap_cid_t category{};
};
// The arguments of each esp_hap_get_setup_payload call; it returns a copy of
// payload().
std::vector<PayloadRequest> payload_requests();
std::string payload();

}  // namespace amity_fake::hap
