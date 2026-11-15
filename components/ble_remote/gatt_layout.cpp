/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "gatt_layout.hpp"

#include <algorithm>
#include <utility>

namespace ble {
namespace {
constexpr uint8_t format_version = 1;
constexpr size_t max_name = 47, max_validators = 4, max_validator_value = 64;
constexpr size_t max_maps = 8, max_reports = 16, max_batteries = 8;

bool valid_subscription(uint8_t value) { return value == 1 || value == 2; }

bool valid(const GattLayout &layout) {
    if (layout.peer_type > 3 || layout.name.size() > max_name) return false;
    if (layout.validators.empty() || layout.validators.size() > max_validators) return false;
    for (const auto &validator : layout.validators)
        if (!validator.handle || validator.value.empty() || validator.value.size() > max_validator_value) return false;
    if (layout.maps.size() > max_maps || layout.reports.size() > max_reports ||
        layout.batteries.size() > max_batteries) return false;
    // Siri input uses Apple's fixed profile; other remotes need HID reports.
    if (layout.siri ? !layout.maps.empty() || !layout.reports.empty() || layout.protocol_mode
                    : layout.reports.empty()) return false;
    for (const auto &map : layout.maps) if (map.empty() || map.size() > UINT16_MAX) return false;
    for (const auto &report : layout.reports)
        if (!report.handle || !report.ccc || report.map >= layout.maps.size() ||
            !valid_subscription(report.subscription)) return false;
    for (const auto &battery : layout.batteries)
        if (!battery.handle || (battery.ccc ? !valid_subscription(battery.subscription) : battery.subscription != 0))
            return false;
    return true;
}

class Writer {
public:
    void u8(uint8_t value) { data.push_back(value); }
    void u16(uint16_t value) { u8(static_cast<uint8_t>(value)); u8(static_cast<uint8_t>(value >> 8)); }
    void bytes(const uint8_t *value, size_t size) { data.insert(data.end(), value, value + size); }
    std::vector<uint8_t> data;
};

class Reader {
public:
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    bool u8(uint8_t &value) {
        if (offset_ >= size_) return false;
        value = data_[offset_++];
        return true;
    }
    bool u16(uint16_t &value) {
        uint8_t low, high;
        if (!u8(low) || !u8(high)) return false;
        value = static_cast<uint16_t>(low | high << 8);
        return true;
    }
    bool bytes(std::vector<uint8_t> &value, size_t size) {
        if (size > size_ - offset_) return false;
        value.assign(data_ + offset_, data_ + offset_ + size);
        offset_ += size;
        return true;
    }
    bool done() const { return offset_ == size_; }
private:
    const uint8_t *data_;
    size_t size_, offset_ = 0;
};
} // namespace

std::vector<uint8_t> encode_gatt_layout(const GattLayout &layout) {
    if (!valid(layout)) return {};
    Writer out;
    out.u8(format_version);
    out.u8(layout.peer_type);
    out.bytes(layout.peer.data(), layout.peer.size());
    out.u8(static_cast<uint8_t>(layout.name.size()));
    out.bytes(reinterpret_cast<const uint8_t *>(layout.name.data()), layout.name.size());
    out.u8(static_cast<uint8_t>((layout.siri ? 1 : 0) | (layout.protocol_mode_without_response ? 2 : 0)));
    out.u16(layout.siri_firmware);
    out.u16(layout.protocol_mode);
    out.u8(static_cast<uint8_t>(layout.validators.size()));
    for (const auto &validator : layout.validators) {
        out.u16(validator.handle);
        out.u8(static_cast<uint8_t>(validator.value.size()));
        out.bytes(validator.value.data(), validator.value.size());
    }
    out.u8(static_cast<uint8_t>(layout.maps.size()));
    for (const auto &map : layout.maps) {
        out.u16(static_cast<uint16_t>(map.size()));
        out.bytes(map.data(), map.size());
    }
    out.u8(static_cast<uint8_t>(layout.reports.size()));
    for (const auto &report : layout.reports) {
        out.u16(report.handle);
        out.u16(report.ccc);
        out.u8(report.map);
        out.u8(report.id);
        out.u8(report.subscription);
    }
    out.u8(static_cast<uint8_t>(layout.batteries.size()));
    for (const auto &battery : layout.batteries) {
        out.u16(battery.handle);
        out.u16(battery.uuid);
        out.u16(battery.ccc);
        out.u8(battery.readable ? 1 : 0);
        out.u8(battery.subscription);
    }
    if (out.data.size() > max_gatt_layout_size) return {};
    return out.data;
}

bool decode_gatt_layout(const uint8_t *data, size_t size, GattLayout &layout) {
    if (!data || !size || size > max_gatt_layout_size) return false;
    Reader in(data, size);
    GattLayout result;
    uint8_t version, length, flags, count;
    std::vector<uint8_t> bytes;
    if (!in.u8(version) || version != format_version || !in.u8(result.peer_type)) return false;
    if (!in.bytes(bytes, result.peer.size())) return false;
    std::copy(bytes.begin(), bytes.end(), result.peer.begin());
    if (!in.u8(length) || !in.bytes(bytes, length)) return false;
    result.name.assign(bytes.begin(), bytes.end());
    if (!in.u8(flags) || flags > 3) return false;
    result.siri = flags & 1;
    result.protocol_mode_without_response = flags & 2;
    if (!in.u16(result.siri_firmware) || !in.u16(result.protocol_mode)) return false;
    if (!in.u8(count) || count > max_validators) return false;
    result.validators.resize(count);
    for (auto &validator : result.validators)
        if (!in.u16(validator.handle) || !in.u8(length) || !in.bytes(validator.value, length)) return false;
    if (!in.u8(count) || count > max_maps) return false;
    result.maps.resize(count);
    for (auto &map : result.maps) {
        uint16_t map_size;
        if (!in.u16(map_size) || !in.bytes(map, map_size)) return false;
    }
    if (!in.u8(count) || count > max_reports) return false;
    result.reports.resize(count);
    for (auto &report : result.reports)
        if (!in.u16(report.handle) || !in.u16(report.ccc) || !in.u8(report.map) || !in.u8(report.id) ||
            !in.u8(report.subscription)) return false;
    if (!in.u8(count) || count > max_batteries) return false;
    result.batteries.resize(count);
    for (auto &battery : result.batteries) {
        uint8_t readable;
        if (!in.u16(battery.handle) || !in.u16(battery.uuid) || !in.u16(battery.ccc) || !in.u8(readable) ||
            readable > 1 || !in.u8(battery.subscription)) return false;
        battery.readable = readable;
    }
    if (!in.done() || !valid(result)) return false;
    layout = std::move(result);
    return true;
}
} // namespace ble
