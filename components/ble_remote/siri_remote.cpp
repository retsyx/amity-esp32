/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// SPDX-License-Identifier: GPL-3.0-or-later
#include "siri_remote.hpp"
#include <cmath>
#include <cstring>

namespace ble {
namespace {
constexpr uint8_t gen2[] = {0x4c, 0x00, 0x07, 0x0d, 0x02, 0x14, 0x03};
constexpr uint8_t gen3[] = {0x4c, 0x00, 0x07, 0x0d, 0x02, 0x15, 0x03};
constexpr uint8_t gen1[] = {0x4c, 0x00, 0x8a, 0x07, 0x66, 0x02};
constexpr uint8_t gen15[] = {0x4c, 0x00, 0x8a, 0x07, 0x6d, 0x02};
template<size_t N> bool prefix(const uint8_t *data, size_t size, const uint8_t (&value)[N]) {
    return size >= N && std::memcmp(data, value, N) == 0;
}
// Apple button bit order; no hold, power, or activity policy lives here.
constexpr RemoteButton logical_buttons[] = {
    RemoteButton::Home, RemoteButton::VolumeUp, RemoteButton::VolumeDown,
    RemoteButton::Select, RemoteButton::Power, RemoteButton::Siri, RemoteButton::Back,
    RemoteButton::Mute, RemoteButton::PlayPause, RemoteButton::Up, RemoteButton::Right,
    RemoteButton::Down, RemoteButton::Left,
};
constexpr double millimeters_per_unit = 31.0 / 1370.0;
constexpr double swipe_threshold_mm = 7.0;
constexpr uint8_t pressure_threshold = 20;
bool whitespace(uint8_t value) { return value == ' ' || (value >= '\t' && value <= '\r'); }
} // namespace

SiriGeneration siri_classify_manufacturer(const uint8_t *data, size_t size) {
    if (!data) return SiriGeneration::None;
    if (prefix(data, size, gen2)) return SiriGeneration::Gen2;
    if (prefix(data, size, gen3)) return SiriGeneration::Gen3;
    if (prefix(data, size, gen1) || prefix(data, size, gen15)) return SiriGeneration::Unsupported;
    return SiriGeneration::None;
}

bool siri_parse_pnp(const uint8_t *data, size_t size, uint16_t &product) {
    if (!data || size != 7) return false;
    const uint16_t vendor = data[1] | (static_cast<uint16_t>(data[2]) << 8);
    if (!((data[0] == 1 && vendor == 0x004c) || (data[0] == 2 && vendor == 0x05ac))) return false;
    product = data[3] | (static_cast<uint16_t>(data[4]) << 8);
    return true;
}

bool siri_parse_firmware(const uint8_t *data, size_t size, uint16_t &revision) {
    if (!data || !size) return false;
    size_t begin = 0, end = size;
    while (begin < end && whitespace(data[begin])) ++begin;
    while (end > begin && whitespace(data[end - 1])) --end;
    if (begin < end && data[begin] == '+') ++begin;
    if (end - begin >= 2 && data[begin] == '0' && (data[begin + 1] == 'x' || data[begin + 1] == 'X')) begin += 2;
    if (begin == end) return false;
    unsigned value = 0;
    for (size_t i = begin; i < end; ++i) {
        const uint8_t c = data[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (value > (0xffff - digit) / 16) return false;
        value = value * 16 + digit;
    }
    revision = static_cast<uint16_t>(value);
    return true;
}

SiriProfile siri_profile(uint16_t firmware) {
    if (firmware >= 0x0083)
        return {0x003a, 0x003e, 0x0036, 0x004e,
                {0x0037, 0x003b, 0x003f, 0x0043, 0x0047, 0x004b, 0x003a}, 7,
                {{{0xf0, 0xbc}, {0xf0, 0xbb}}}, 2, 0x3b, 0x2f, 0x2b};
    return {0x0039, 0x003d, 0x0035, 0x004d,
            {0x003a, 0x003e, 0x0036}, 3,
            {{{0xf0, 0x00}, {0, 0}}}, 1, 0xab, 0xaf, 0xbb};
}

int siri_charging_state(const SiriProfile &profile, const uint8_t *data, size_t size) {
    if (!data || size != 1) return -1;
    if (data[0] == profile.charging || data[0] == profile.plugged_in) return 1;
    if (data[0] == profile.discharging) return 0;
    return -1;
}

void SiriInputAdapter::reset_touch() {
    touch_active_ = false;
    primary_axis_ = -1;
}

void SiriInputAdapter::activity_changed() {
    if (physical_touches_) touch_blocked_ = true;
    reset_touch();
}

void SiriInputAdapter::buttons(const uint8_t *data, size_t size, InputStamp stamp) {
    if (!data || !size || size > 4) return;
    uint16_t next = data[0];
    if (size > 1) next |= static_cast<uint16_t>(data[1]) << 8;
    next &= 0x1fff;
    const auto previous = buttons_;
    buttons_ = next;
    if (next) activity_changed();
    // Selectors release before Power. New presses in an action's report stay
    // quarantined by the shared controller; reconcile only after the batch.
    constexpr size_t order[] = {0, 1, 2, 7, 3, 6, 8, 9, 10, 11, 12, 4, 5};
    ButtonEvent events[26];
    size_t count = 0;
    for (size_t i : order) if ((previous & (1u << i)) && !(next & (1u << i)))
        events[count++] = ButtonEvent{static_cast<uint32_t>(i), logical_buttons[i], false};
    for (size_t i : order) if ((next & (1u << i)) && !(previous & (1u << i)))
        events[count++] = ButtonEvent{static_cast<uint32_t>(i), logical_buttons[i], true};
    controller_.buttons(RemoteProfile::Siri, events, count, stamp);
}

SiriInputAdapter::Touch SiriInputAdapter::decode_touch(const uint8_t *data, uint16_t timestamp) {
    int x = ((data[1] & 0x0f) << 8) | data[0];
    int y = (data[2] << 4) | ((data[1] & 0xf0) >> 4);
    if (x & 0x800) x -= 0x1000;
    if (y & 0x800) y -= 0x1000;
    return {timestamp, x + 360, y + 360, data[5], static_cast<uint8_t>(1 - ((data[6] & 0x08) >> 3))};
}

void SiriInputAdapter::touches(const uint8_t *data, size_t size, InputStamp stamp) {
    if (!data || (size != 11 && size != 18) || data[0] != 0x32) return;
    const uint16_t timestamp = data[1] | (static_cast<uint16_t>(data[2]) << 8);
    const Touch fingers[] = {decode_touch(data + 4, timestamp), size == 18 ? decode_touch(data + 11, timestamp) : Touch{}};
    const size_t count = size == 18 ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << fingers[i].id);
        if (fingers[i].pressure >= pressure_threshold) physical_touches_ |= bit;
        else physical_touches_ &= static_cast<uint8_t>(~bit);
    }
    if (!controller_.active() || buttons_ || !controller_.accepts(stamp)) {
        touch_blocked_ = physical_touches_ != 0;
        reset_touch();
        return;
    }
    if (touch_blocked_) {
        if (!physical_touches_) touch_blocked_ = false;
        reset_touch();
        return;
    }
    if (physical_touches_ == 3) { touch_blocked_ = true; reset_touch(); return; }
    for (size_t i = 0; i < count; ++i) {
        const auto &touch = fingers[i];
        if (!touch_active_) {
            if (touch.pressure < pressure_threshold) continue;
            touch_active_ = true;
            start_touch_ = last_touch_ = touch;
            primary_axis_ = -1;
            continue;
        }
        if (touch.id != start_touch_.id) {
            if (touch.pressure >= pressure_threshold) { reset_touch(); return; }
            continue;
        }
        const bool ended = touch.pressure < pressure_threshold;
        int axis = primary_axis_, repeats = 0;
        if (ended) {
            const uint16_t elapsed = static_cast<uint16_t>(touch.timestamp - start_touch_.timestamp);
            for (int candidate = axis >= 0 ? axis : 0; candidate < 2; ++candidate) {
                const int delta = candidate == 0 ? touch.x - start_touch_.x : touch.y - start_touch_.y;
                const double velocity = elapsed ? delta * millimeters_per_unit * 2000.0 / elapsed : 31.0;
                const double momentum = velocity * std::fabs(velocity) / (500.0 * swipe_threshold_mm);
                repeats = momentum >= 6 ? 6 : momentum <= -6 ? -6 : static_cast<int>(momentum);
                if (repeats || axis >= 0) { axis = candidate; break; }
            }
            reset_touch();
        } else {
            for (int candidate = axis >= 0 ? axis : 0; candidate < 2; ++candidate) {
                const int delta = candidate == 0 ? touch.x - last_touch_.x : touch.y - last_touch_.y;
                const double distance = delta * millimeters_per_unit;
                if (std::fabs(distance) > swipe_threshold_mm) {
                    repeats = static_cast<int>(distance / swipe_threshold_mm);
                    axis = primary_axis_ = candidate;
                    last_touch_ = touch;
                    break;
                }
                if (axis >= 0) break;
            }
        }
        if (!repeats) continue;
        const uint8_t key = axis == 0 ? (repeats < 0 ? 0x03 : 0x04) : (repeats < 0 ? 0x02 : 0x01);
        controller_.swipe(static_cast<RemoteButton>(key), static_cast<uint8_t>(repeats < 0 ? -repeats : repeats), stamp);
    }
}

void SiriInputAdapter::cancel() {
    // Retain the raw mask so an unchanged full-state report cannot replay a
    // held button. An empty reconnect report clears the physical mask.
    if (physical_touches_) touch_blocked_ = true;
    // A release may be lost offline. Keep quarantine, but let the next reports
    // establish contact IDs so a missing old contact cannot block forever.
    physical_touches_ = 0;
    reset_touch();
}
} // namespace ble
