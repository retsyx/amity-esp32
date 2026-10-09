/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "battery_status.hpp"
#include <cassert>
#include <initializer_list>
#include <iostream>
int main() {
    ble::BatteryStatus state;
    auto read = [&](uint16_t uuid, std::initializer_list<uint8_t> bytes) {
        return ble::parse_battery(uuid, bytes.begin(), bytes.size(), state);
    };
    assert(state.percent == -1 && state.charging == -1);
    for (unsigned value = 0; value <= 255; ++value) {
        uint8_t byte = value;
        assert(ble::parse_battery(0x2a19, &byte, 1, state) == (value <= 100));
    }
    assert(read(0x2a19, {87}) && state.percent == 87);
    assert(read(0x2a1a, {0x3b}) && state.charging == 1);
    assert(read(0x2a1a, {0x2f}) && state.charging == 0);
    assert(read(0x2a1a, {0x0b}) && state.charging == -1);
    assert(read(0x2bed, {2,0x23,0,86}) && state.percent == 86 && state.charging == 1);
    assert(read(0x2bed, {0,0x41,0}) && state.percent == 86 && state.charging == 0);
    assert(read(0x2bed, {7,0x61,0,0x34,0x12,100,0}) && state.percent == 100 && state.charging == 0);
    assert(read(0x2bed, {2,1,0,255}) && state.percent == -1 && state.charging == -1);
    assert(read(0x2bed, {2,0x23,0,65}));
    for (auto bytes : {std::initializer_list<uint8_t>{}, {2}, {2,0x23}, {2,0x23,0},
                       {2,0x23,0,101}, {2,0x23,0,254}, {7,0x23,0,1,2,3}, {0x80,0x23,0}}) {
        assert(!read(0x2bed, bytes));
        assert(state.percent == 65 && state.charging == 1);
    }
    assert(read(0x2bed, {2,0x22,0,65}) && state.percent == -1 && state.charging == -1); // Absent.
    assert(!read(0x2a19, {2,3}) && !read(0x2a1a,{1,2}) && !read(0xffff,{0}));
    assert(!ble::parse_battery(0x2a19, nullptr, 1, state));
    auto reported = ble::reported_battery(false, false, {});
    assert(reported.percent == 100 && reported.charging == 0);
    reported = ble::reported_battery(true, true, {54,1});
    assert(reported.percent == 54 && reported.charging == 1);
    reported = ble::reported_battery(true, false, reported);
    assert(reported.percent == -1 && reported.charging == -1);
    reported = ble::reported_battery(false, true, {54,1}); // Unpair while link termination is pending.
    assert(reported.percent == 100 && reported.charging == 0);
    reported = ble::reported_battery(true, true, {}); // No readings yet after reconnect.
    assert(reported.percent == -1 && reported.charging == -1);
    std::cout << "PASS Battery Service level, charging, unknown, absent and malformed readings\n";
}
