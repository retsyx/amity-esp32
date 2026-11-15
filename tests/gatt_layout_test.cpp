/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "gatt_layout.hpp"
#include <cassert>
#include <iostream>

namespace {
ble::GattLayout keyboard() {
    ble::GattLayout layout;
    layout.peer_type = 1;
    layout.peer = {0xd4, 0x0f, 0xc8, 0x30, 0x42, 0xee};
    layout.name = "Couch Clicker";
    layout.protocol_mode = 0x1d;
    layout.protocol_mode_without_response = true;
    layout.validators = {{0x0007, std::vector<uint8_t>(16, 0xa5)}};
    layout.maps = {{0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0xc0}};
    layout.reports = {{0x0022, 0x0023, 0, 1, 1}, {0x0026, 0x0027, 0, 2, 1}};
    layout.batteries = {{0x0012, 0x2a19, 0x0013, true, 1}, {0x0016, 0x2a1a, 0, false, 0}};
    return layout;
}

ble::GattLayout siri() {
    ble::GattLayout layout;
    layout.peer = {1, 2, 3, 4, 5, 6};
    layout.name = "Siri Remote, Gen 3";
    layout.siri = true;
    layout.siri_firmware = 0x83;
    layout.validators = {{0x0040, {'0', 'x', '8', '3'}}, {0x0007, std::vector<uint8_t>(16, 1)}};
    layout.batteries = {{0x0030, 0x2a19, 0x0031, true, 1}};
    return layout;
}

bool same(const ble::GattLayout &a, const ble::GattLayout &b) {
    if (a.peer_type != b.peer_type || a.peer != b.peer || a.name != b.name || a.siri != b.siri ||
        a.siri_firmware != b.siri_firmware || a.protocol_mode != b.protocol_mode ||
        a.protocol_mode_without_response != b.protocol_mode_without_response || a.maps != b.maps ||
        a.validators.size() != b.validators.size() || a.reports.size() != b.reports.size() ||
        a.batteries.size() != b.batteries.size()) return false;
    for (size_t i = 0; i < a.validators.size(); ++i)
        if (a.validators[i].handle != b.validators[i].handle || a.validators[i].value != b.validators[i].value) return false;
    for (size_t i = 0; i < a.reports.size(); ++i) {
        const auto &x = a.reports[i], &y = b.reports[i];
        if (x.handle != y.handle || x.ccc != y.ccc || x.map != y.map || x.id != y.id || x.subscription != y.subscription) return false;
    }
    for (size_t i = 0; i < a.batteries.size(); ++i) {
        const auto &x = a.batteries[i], &y = b.batteries[i];
        if (x.handle != y.handle || x.uuid != y.uuid || x.ccc != y.ccc || x.readable != y.readable ||
            x.subscription != y.subscription) return false;
    }
    return true;
}

void round_trip(const ble::GattLayout &layout) {
    const auto encoded = ble::encode_gatt_layout(layout);
    assert(!encoded.empty());
    ble::GattLayout decoded;
    assert(ble::decode_gatt_layout(encoded.data(), encoded.size(), decoded));
    assert(same(layout, decoded));
    // Every truncation and any trailing byte is rejected without changing the output.
    for (size_t size = 0; size < encoded.size(); ++size) {
        ble::GattLayout untouched = siri();
        assert(!ble::decode_gatt_layout(encoded.data(), size, untouched));
        assert(same(untouched, siri()));
    }
    auto longer = encoded;
    longer.push_back(0);
    assert(!ble::decode_gatt_layout(longer.data(), longer.size(), decoded));
    auto version = encoded;
    ++version[0];
    assert(!ble::decode_gatt_layout(version.data(), version.size(), decoded));
}

void rejected(void (*change)(ble::GattLayout &), bool from_siri = false) {
    auto layout = from_siri ? siri() : keyboard();
    change(layout);
    assert(ble::encode_gatt_layout(layout).empty());
}
} // namespace

int main() {
    round_trip(keyboard());
    round_trip(siri());

    // A layout that cannot be validated or used for input is never stored.
    rejected([](ble::GattLayout &l) { l.validators.clear(); });
    rejected([](ble::GattLayout &l) { l.validators[0].value.clear(); });
    rejected([](ble::GattLayout &l) { l.validators[0].handle = 0; });
    rejected([](ble::GattLayout &l) { l.reports.clear(); });
    rejected([](ble::GattLayout &l) { l.reports[1].map = 1; });
    rejected([](ble::GattLayout &l) { l.reports[0].ccc = 0; });
    rejected([](ble::GattLayout &l) { l.reports[0].subscription = 0; });
    rejected([](ble::GattLayout &l) { l.batteries[1].subscription = 1; });
    rejected([](ble::GattLayout &l) { l.batteries[0].subscription = 0; });
    rejected([](ble::GattLayout &l) { l.maps[0].clear(); });
    rejected([](ble::GattLayout &l) { l.maps[0].resize(ble::max_gatt_layout_size); });
    rejected([](ble::GattLayout &l) { l.name.assign(48, 'x'); });
    rejected([](ble::GattLayout &l) { l.peer_type = 4; });
    rejected([](ble::GattLayout &l) { l.reports = keyboard().reports; }, true);
    rejected([](ble::GattLayout &l) { l.protocol_mode = 1; }, true);

    auto most = keyboard();
    most.name.assign(47, 'x');
    assert(!ble::encode_gatt_layout(most).empty());

    ble::GattLayout decoded;
    assert(!ble::decode_gatt_layout(nullptr, 1, decoded));
    std::cout << "gatt_layout_test passed\n";
}
