// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Python Amity's remote, adapter, gestures and hub implementations.
// Its Apple report decoding derives from Yanndroid (https://github.com/Yanndroid).
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
// Bit order is the remote's wire format. Play/pause is translated by the hub.
constexpr uint8_t cec_keys[] = {0x09, 0x41, 0x42, 0x00, 0x40, 0x35, 0x0d,
                               0x43, 0x61, 0x01, 0x04, 0x02, 0x03};
constexpr int power_index = 4;
constexpr int selector(size_t index) {
    return index == 3 ? 0 : index >= 9 && index <= 12 ? static_cast<int>(index) - 8 : -1;
}
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

void SiriController::emit(SiriCommandType type, uint8_t value, uint8_t count) {
    if (callback_) callback_(SiriCommand{type, value, count}, context_);
}

void SiriController::reset_touch() {
    touch_active_ = false;
    primary_axis_ = -1;
}

void SiriController::suppress_held() {
    for (auto &key : held_) if (key.down) key.suppressed = true;
    macro_selector_ = -1;
    if (physical_touches_) touch_blocked_ = true;
    reset_touch();
    reconcile();
}

void SiriController::activity_changed(bool activity_active) {
    active_ = activity_active;
    suppress_held();
}

void SiriController::sync_mode(bool activity_active) {
    if (active_ != activity_active) activity_changed(activity_active);
}

void SiriController::reconcile() {
    int latest = -1;
    if (active_) for (size_t i = 0; i < held_.size(); ++i) {
        const auto &key = held_[i];
        if (!key.down || key.suppressed || i == power_index || static_cast<int>(i) == macro_selector_) continue;
        if (latest < 0 || held_[static_cast<size_t>(latest)].order < key.order) latest = static_cast<int>(i);
    }
    const int next = latest >= 0 ? cec_keys[latest] : -1;
    if (next == current_key_) return;
    if (current_key_ >= 0) emit(SiriCommandType::Release);
    current_key_ = next;
    if (current_key_ >= 0) emit(SiriCommandType::Press, static_cast<uint8_t>(current_key_));
}

void SiriController::release(size_t index, uint64_t now) {
    auto &key = held_[index];
    if (!key.down) return;
    const bool suppressed = key.suppressed;
    const uint64_t started = key.started;
    key.down = false;
    if (suppressed) return;
    const bool long_press = now >= started && now - started >= long_press_us;
    if (macro_selector_ >= 0) {
        if (static_cast<int>(index) != macro_selector_ && index != power_index) return;
        const int macro = macro_selector_;
        const auto &function = held_[static_cast<size_t>(macro)];
        const uint64_t macro_start = static_cast<int>(index) == macro ? started : function.started;
        const bool long_macro = now >= macro_start && now - macro_start >= long_press_us;
        completed_action_ = true;
        suppress_held();
        emit(SiriCommandType::Activity, static_cast<uint8_t>(selector(static_cast<size_t>(macro)) + (long_macro ? 5 : 0)));
    } else if (!active_ && selector(index) >= 0) {
        completed_action_ = true;
        suppress_held();
        emit(SiriCommandType::Activity, static_cast<uint8_t>(selector(index) + (long_press ? 5 : 0)));
    } else if (index == power_index) {
        completed_action_ = true;
        suppress_held();
        if (!active_ && !long_press) emit(SiriCommandType::Activity, 0);
        else emit(long_press ? SiriCommandType::PowerLong : SiriCommandType::PowerShort);
    }
}

void SiriController::buttons(const uint8_t *data, size_t size, uint64_t now, bool activity_active) {
    if (!data || !size || size > 4) return;
    uint16_t next = data[0];
    if (size > 1) next |= static_cast<uint16_t>(data[1]) << 8;
    next &= 0x1fff;
    sync_mode(activity_active);
    blocked_ &= next;
    const uint16_t previous = buttons_;
    buttons_ = next;
    completed_action_ = false;
    if (next) {
        if (physical_touches_) touch_blocked_ = true;
        reset_touch();
    }
    // Release activity selectors before power, matching Python's adapter key order.
    constexpr size_t release_order[] = {0, 1, 2, 7, 3, 6, 8, 9, 10, 11, 12, 4, 5};
    for (size_t i : release_order) if ((previous & (1u << i)) && !(next & (1u << i))) release(i, now);
    for (size_t i : release_order) {
        if (!(next & (1u << i)) || (previous & (1u << i))) continue;
        held_[i] = Held{true, completed_action_ || (blocked_ & (1u << i)) != 0, now, ++next_order_};
    }
    if (active_ && macro_selector_ < 0 && held_[power_index].down && !held_[power_index].suppressed) {
        constexpr size_t selectors[] = {3, 9, 10, 11, 12};
        for (size_t i : selectors) if (held_[i].down && !held_[i].suppressed) {
            macro_selector_ = static_cast<int>(i);
            break;
        }
    }
    reconcile();
}

SiriController::Touch SiriController::decode_touch(const uint8_t *data, uint16_t timestamp) {
    int x = ((data[1] & 0x0f) << 8) | data[0];
    int y = (data[2] << 4) | ((data[1] & 0xf0) >> 4);
    if (x & 0x800) x -= 0x1000;
    if (y & 0x800) y -= 0x1000;
    return {timestamp, x + 360, y + 360, data[5], static_cast<uint8_t>(1 - ((data[6] & 0x08) >> 3))};
}

void SiriController::touches(const uint8_t *data, size_t size, bool activity_active) {
    if (!data || (size != 11 && size != 18) || data[0] != 0x32) return;
    sync_mode(activity_active);
    const uint16_t timestamp = data[1] | (static_cast<uint16_t>(data[2]) << 8);
    const Touch fingers[] = {decode_touch(data + 4, timestamp), size == 18 ? decode_touch(data + 11, timestamp) : Touch{}};
    const size_t count = size == 18 ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << fingers[i].id);
        if (fingers[i].pressure >= pressure_threshold) physical_touches_ |= bit;
        else physical_touches_ &= static_cast<uint8_t>(~bit);
    }
    if (!active_ || buttons_) {
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
        emit(SiriCommandType::Swipe, key, static_cast<uint8_t>(repeats < 0 ? -repeats : repeats));
    }
}

void SiriController::cancel() {
    blocked_ |= buttons_;
    buttons_ = 0;
    for (auto &key : held_) key = {};
    macro_selector_ = -1;
    if (physical_touches_) touch_blocked_ = true;
    // A release may be lost while disconnected or when the queue overflows.
    // Preserve quarantine, but let subsequent reports establish contact IDs.
    physical_touches_ = 0;
    reset_touch();
    reconcile();
}
} // namespace ble
