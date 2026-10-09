/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <cstddef>
#include <cstdint>

namespace ble {
constexpr uint16_t battery_level_uuid = 0x2a19;
constexpr uint16_t battery_power_state_uuid = 0x2a1a;
constexpr uint16_t battery_level_status_uuid = 0x2bed;
struct BatteryStatus {
    int percent = -1;
    int charging = -1; // -1 unknown, 0 not charging, 1 charging.
};
inline BatteryStatus reported_battery(bool paired, bool connected, BatteryStatus observed) {
    if (!paired) return {100, 0};
    if (!connected) return {};
    return observed;
}
inline bool battery_characteristic(uint16_t uuid) {
    return uuid == battery_level_uuid || uuid == battery_power_state_uuid || uuid == battery_level_status_uuid;
}
// Bluetooth Battery Service (BAS 1.1 and legacy Battery Power State).
// BLS flags: identifier, level, additional status. Power bits 5..6 are charging.
// Legacy charging bits 4..5: 0 unknown, 1 unsupported, 2 no, 3 yes.
// A malformed update never overwrites prior readings.
inline bool parse_battery(uint16_t uuid, const uint8_t *data, size_t size, BatteryStatus &state) {
    if (!data) return false;
    auto next = state;
    if (uuid == battery_level_uuid) {
        if (size != 1 || data[0] > 100) return false;
        next.percent = data[0];
    } else if (uuid == battery_power_state_uuid) {
        if (size != 1) return false;
        const auto present = data[0] & 3, charging = (data[0] >> 4) & 3;
        next.charging = present == 2 || charging < 2 ? -1 : charging == 3;
        if (present == 2) next.percent = -1;
    } else if (uuid == battery_level_status_uuid) {
        if (size < 3 || (data[0] & 0xf8)) return false;
        const size_t level_offset = 3 + ((data[0] & 1) ? 2 : 0);
        const size_t expected = level_offset + ((data[0] & 2) ? 1 : 0) + ((data[0] & 4) ? 1 : 0);
        if (size != expected) return false;
        const uint16_t power = data[1] | (data[2] << 8);
        const unsigned charge = (power >> 5) & 3;
        next.charging = !(power & 1) || !charge ? -1 : charge == 1;
        if (data[0] & 2) {
            const auto percent = data[level_offset];
            if (percent > 100 && percent != 255) return false;
            next.percent = percent == 255 ? -1 : percent;
        }
        if (!(power & 1)) next.percent = -1;
    } else return false;
    state = next;
    return true;
}
}
