/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ble {
// The GATT layout of the bonded remote, saved after a full discovery so a
// reconnect can accept input without discovering again. Validators are values
// that identify the remote's database (Database Hash, Siri firmware revision);
// each must read back unchanged before the layout is reused.
struct GattLayout {
    struct Validator {
        uint16_t handle = 0;
        std::vector<uint8_t> value;
    };
    struct Report {
        uint16_t handle = 0, ccc = 0;
        uint8_t map = 0, id = 0, subscription = 0;
    };
    struct Battery {
        uint16_t handle = 0, uuid = 0, ccc = 0; // ccc is 0 when it cannot be subscribed.
        bool readable = false;
        uint8_t subscription = 0;
    };
    uint8_t peer_type = 0;
    std::array<uint8_t, 6> peer{};
    std::string name;
    bool siri = false;
    uint16_t siri_firmware = 0;
    uint16_t protocol_mode = 0; // 0 when absent.
    bool protocol_mode_without_response = false;
    std::vector<Validator> validators;
    std::vector<std::vector<uint8_t>> maps;
    std::vector<Report> reports;
    std::vector<Battery> batteries;
};

constexpr size_t max_gatt_layout_size = 4096;
// Returns an empty buffer for a layout that cannot be stored.
std::vector<uint8_t> encode_gatt_layout(const GattLayout &layout);
bool decode_gatt_layout(const uint8_t *data, size_t size, GattLayout &layout);
} // namespace ble
