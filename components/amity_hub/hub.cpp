/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity/hub.hpp"
#include <algorithm>
#include <utility>

namespace amity {
namespace {
bool usable_front_physical_address(uint16_t address) {
    // Zero belongs to the root TV; FFFF is the CEC invalid/unknown sentinel.
    if (address == 0 || address == 0xffff) return false;
    bool zero = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const auto port = (address >> shift) & 15;
        if (zero && port) return false;
        if (!port) zero = true;
    }
    return true;
}
std::vector<std::string> names(const Activity &a) {
    std::vector<std::string> result;
    // A device can fill several roles, but each power transition applies once.
    for (const auto &name : {a.display, a.source, a.audio, a.switch_device})
        if (!name.empty() && std::find(result.begin(), result.end(), name) == result.end()) result.push_back(name);
    return result;
}
}
bool same_executable_configuration(const std::vector<Activity> &a, const std::vector<Quirk> &qa,
                                   const std::vector<Activity> &b, const std::vector<Quirk> &qb) {
    if (a.size() != b.size() || qa.size() != qb.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].display != b[i].display || a[i].source != b[i].source || a[i].audio != b[i].audio ||
            a[i].switch_device != b[i].switch_device || a[i].switch_input != b[i].switch_input) return false;
    for (size_t i = 0; i < qa.size(); ++i)
        if (qa[i].vendor_id != qb[i].vendor_id || qa[i].power_on != qb[i].power_on || qa[i].power_off != qb[i].power_off) return false;
    return true;
}
void Hub::configure(std::vector<Activity> activities, std::vector<Quirk> quirks) {
    if (!same_executable_configuration(activities_, quirks_, activities, quirks)) {
        // Installation is not an activity intent. Fence old input and suspend
        // its logical route without powering off, waking or routing equipment.
        release_key();
        selected_ = -1;
        audio_query_pending_ = false;
        audio_status_known_ = false;
        ++activity_generation_;
    }
    activities_ = std::move(activities);
    quirks_ = std::move(quirks);
    // Scope memory belongs to configured selectors, not every untrusted OSD
    // string ever seen. Semantic configuration already fenced removed intent.
    name_scopes_.erase(std::remove_if(name_scopes_.begin(), name_scopes_.end(), [&](const auto& scope) {
        return !configured_name(scope.first);
    }), name_scopes_.end());
    for (auto& d : devices_) if (d.resolved && configured_name(d.name)) {
        auto scope = std::find_if(name_scopes_.begin(), name_scopes_.end(), [&](const auto& item) { return item.first == d.name; });
        if (scope != name_scopes_.end()) d.resolved = scope->second == d.bus;
        else if (name_scopes_.size() < 64) name_scopes_.push_back({d.name, d.bus});
        else d.resolved = false;
    }
}
bool Hub::configured_name(const std::string& name) const {
    if (name.empty()) return false;
    for (const auto& activity : activities_) for (const auto* candidate : {&activity.display, &activity.source, &activity.audio, &activity.switch_device})
        if (*candidate == name) return true;
    return false;
}
void Hub::set_addresses(uint8_t front, uint8_t back, uint16_t physical) {
    front_ = front; back_ = back; physical_ = physical;
}
void Hub::announce(BusSide side) {
    const bool front = side == BusSide::Front;
    if ((front ? front_ : back_) == 15 || (front && physical_ == 0xffff)) return;
    const uint16_t pa = front ? physical_ : 0;
    // Linux announces features first for a CEC 2.0 adapter, then its physical
    // address and vendor. Preserve both payloads and ordering.
    send(side, 15, {0xa6, 6, static_cast<uint8_t>(front ? 0x10 : 0x80), 0, 0});
    send(side, 15, {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(front ? 4 : 0)});
    send(side, 15, {0x87, 0, 0, 0});
}
Snapshot Hub::snapshot() const {
    Snapshot s;
    s.activities = activities_; s.devices = devices_; s.current_activity = selected_;
    s.activity_generation = activity_generation_;
    s.binding_generation = binding_generation_; s.uncertain_releases = uncertain_releases_;
    s.front_address = front_; s.back_address = back_; s.front_physical_address = physical_;
    s.ready = front_ < 15 && back_ < 15 && physical_ != 0xffff;
    s.audio_status_known = audio_status_known_; s.muted = muted_; s.volume = volume_;
    return s;
}
Activity Hub::current() const {
    return selected_ >= 0 && static_cast<size_t>(selected_) < activities_.size() ? activities_[selected_] : Activity{};
}
Device *Hub::find(const std::string &name) {
    if (name.empty()) return nullptr;
    Device *match = nullptr;
    for (auto &d : devices_) if (d.name == name) {
        if (!d.resolved || match) { transport_.request_scan(); return nullptr; }
        match = &d;
    }
    if (match) return match;
    transport_.request_scan();
    return nullptr;
}
const Device *Hub::known(const std::string &name) const {
    const Device *match = nullptr;
    if (!name.empty()) for (const auto &d : devices_) if (d.name == name) {
        if (!d.resolved || match) return nullptr;
        match = &d;
    }
    return match;
}
bool Hub::send(BusSide side, uint8_t dest, std::initializer_list<uint8_t> data) {
    return transport_.send(side, dest, data.begin(), data.size());
}
bool Hub::activity_needs_discovery(int index) const {
    if (index < -1 || index >= static_cast<int>(activities_.size())) return false;
    for (const auto &name : names(current())) if (!name.empty() && !known(name)) return true;
    if (index >= 0) for (const auto &name : names(activities_[index])) if (!name.empty() && !known(name)) return true;
    return false;
}
bool Hub::key_needs_discovery(uint8_t key) const {
    if (key == 0x40) return activity_needs_discovery(-1);
    const auto a = current();
    const auto &name = ((key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66) ? a.audio : a.source;
    return !name.empty() && !known(name);
}
void Hub::power(const Device &device, bool on, bool quirk) {
    if (quirk) for (const auto &q : quirks_) {
        if (q.vendor_id != device.vendor_id) continue;
        const auto &data = on ? q.power_on : q.power_off;
        if (data.empty() || data.size() > 15) continue;
        transport_.send(device.bus, device.address, data.data(), data.size());
        if (data[0] == 0x44) send(device.bus, device.address, {0x45});
        return;
    }
    if (!on) send(device.bus, device.address, {0x36});
    else if (device.type == 0) send(device.bus, device.address, {0x04});
    else {
        send(device.bus, device.address, {0x44, 0x6d});
        send(device.bus, device.address, {0x45});
    }
}
void Hub::active_source() {
    if (physical_ == 0xffff) return;
    send(BusSide::Front, 15, {0x82, static_cast<uint8_t>(physical_ >> 8), static_cast<uint8_t>(physical_)});
}
void Hub::input(const Activity &activity) {
    if (auto *d = find(activity.source)) {
        send(d->bus, 15, {0x86, static_cast<uint8_t>(d->physical_address >> 8), static_cast<uint8_t>(d->physical_address)});
        return;
    }
    if (!activity.source.empty()) return; // Unresolved source cannot authorize a fallback route.
    if (!activity.switch_device.empty()) {
        auto *d = find(activity.switch_device);
        if (!d) return;
        send(d->bus, d->address, {0x44, 0x69, activity.switch_input});
        send(d->bus, d->address, {0x45});
    }
    active_source();
}
bool Hub::select_activity(int index) {
    if (index < -1 || index >= static_cast<int>(activities_.size())) return false;
    if (index >= 0) for (const auto& name : names(activities_[index])) if (!known(name)) {
        transport_.request_scan(); return false; // Reject before partial power/routing effects.
    }
    if (index == selected_) { fix_current_activity(); return true; }
    release_key();
    const auto before_activity = current();
    const auto before = names(before_activity);
    const auto after_activity = index < 0 ? Activity{} : activities_[index];
    const auto after = names(after_activity);
    for (const auto &name : before) if (std::find(after.begin(), after.end(), name) == after.end()) {
        if (auto *d = find(name)) power(*d, false, index == -1);
    }
    for (const auto &name : after) if (std::find(before.begin(), before.end(), name) == before.end()) {
        if (auto *d = find(name)) power(*d, true);
    }
    input(after_activity);
    selected_ = index;
    ++activity_generation_;
    audio_status_known_ = false; volume_ = 127;
    request_audio_status();
    if (index < 0 || before_activity.source != after_activity.source) playing_ = false;
    return true;
}
void Hub::fix_current_activity() {
    const auto a = current();
    for (const auto& name : names(a)) if (!known(name)) { transport_.request_scan(); return; }
    for (const auto &name : names(a)) if (auto *d = find(name)) power(*d, true);
    input(a);
    request_audio_status();
}
void Hub::request_audio_status() {
    if (const auto *d = known(current().audio)) {
        audio_query_pending_ = !send(d->bus, d->address, {0x71});
        if (audio_query_pending_) audio_query_due_ = now_us_ + 500000;
    } else audio_query_pending_ = false;
}
void Hub::standby() {
    if (selected_ < 0) force_standby();
    else select_activity(-1);
    playing_ = false;
}
void Hub::force_standby() {
    send(BusSide::Front, 15, {0x36});
    send(BusSide::Back, 15, {0x36});
}
bool Hub::press_key(uint8_t key, bool repeat) {
    const auto a = current();
    if (key == 0x40) { standby(); return true; }
    if (release_pending_ && !release_key()) return false;
    const uint8_t raw_key = key;
    // Default Python hub behavior emulates the play/pause toggle.
    if (key == 0x61) {
        if (key_held_ && held_raw_key_ == raw_key) key = held_key_;
        else key = playing_ ? 0x46 : 0x44;
    }
    const bool audio_key = (key >= 0x41 && key <= 0x43) || key == 0x65 || key == 0x66;
    auto *d = find(audio_key ? a.audio : a.source);
    if (!d) return false;
    if (key_held_ && (held_bus_ != d->bus || held_address_ != d->address) && !release_key()) return false;
    if (!send(d->bus, d->address, {0x44, key})) return false;
    // Explicit transport commands and emulated toggles share the same last
    // accepted state. Held toggles keep emitting their original mapped key.
    if (key == 0x44) playing_ = true;
    else if (key == 0x46 || key == 0x45) playing_ = false;
    held_bus_ = d->bus; held_address_ = d->address; key_held_ = true;
    held_raw_key_ = raw_key; held_key_ = key;
    held_audio_ = audio_key;
    if (audio_key) audio_status_known_ = false;
    if (!repeat) release_key();
    return true;
}
bool Hub::release_key() {
    if (!key_held_) return true;
    const bool admitted = send(held_bus_, held_address_, {0x45});
    // The final-release decision is consumed even when the ninth driver slot
    // is occupied. No later tick, up or new key retries a dropped release.
    key_held_ = false;
    release_pending_ = false;
    if (held_audio_ && admitted) {
        audio_query_pending_ = true;
        audio_query_due_ = now_us_ + 500000;
    }
    held_audio_ = false;
    if (!admitted) ++uncertain_releases_;
    return admitted;
}
void Hub::end_key_stream() { if (key_held_) release_pending_ = true; }
void Hub::tick(uint64_t now_us) {
    now_us_ = now_us;
    if (release_pending_) release_key();
    if (audio_query_pending_ && now_us_ >= audio_query_due_ && !held_audio_) request_audio_status();
}
void Hub::invalidate_endpoint(BusSide side, uint8_t address) {
    transport_.invalidate_binding(side, address);
    ++binding_generation_;
    ++activity_generation_; // Fence old normalized input while keeping intent selected.
    if (key_held_ && held_bus_ == side && held_address_ == address) {
        // We cannot safely release through an address whose appliance may have
        // changed. Never retarget the old hold to a newly discovered endpoint.
        ++uncertain_releases_;
        key_held_ = false; release_pending_ = false; held_audio_ = false;
    }
    audio_query_pending_ = false;
    audio_status_known_ = false;
    for (auto &d : devices_) if (d.bus == side && d.address == address) d.resolved = false;
}
void Hub::clear_devices() {
    for (const auto &d : devices_) if (d.resolved) invalidate_endpoint(d.bus, d.address);
    devices_.clear(); discovery_.clear(); name_scopes_.clear(); discovering_ = false;
}
void Hub::begin_discovery() { discovery_.clear(); discovered_physical_ = 0xffff; discovering_ = true; }
void Hub::determine_front_physical_address(uint16_t fallback) {
    if (physical_ != 0xffff) return;
    // A parent precedes its descendants numerically (2.0.0.0 < 2.1.0.0).
    // Keep the chosen address, including fallback, for the rest of this boot.
    physical_ = discovered_physical_ != 0xffff ? discovered_physical_ : fallback;
}
void Hub::finish_discovery() {
    if (!discovering_) return;
    // A bounded scan, rather than arrival order or remembered LA, establishes
    // uniqueness. Names and PAs never erase another responding native device.
    for (auto &d : discovery_) {
        const auto count = std::count_if(discovery_.begin(), discovery_.end(), [&](const Device& candidate) { return !d.name.empty() && candidate.name == d.name; });
        auto scope = std::find_if(name_scopes_.begin(), name_scopes_.end(), [&](const auto& item) { return item.first == d.name; });
        d.resolved = !d.name.empty() && count == 1 && (scope == name_scopes_.end() || scope->second == d.bus);
        if (d.resolved && scope == name_scopes_.end() && configured_name(d.name)) {
            if (name_scopes_.size() < 64) name_scopes_.push_back({d.name, d.bus});
            else d.resolved = false;
        }
    }
    for (const auto &old : devices_) if (old.resolved) {
        const auto unchanged = std::find_if(discovery_.begin(), discovery_.end(), [&](const Device& d) {
            return d.resolved && d.bus == old.bus && d.address == old.address && d.name == old.name && d.physical_address == old.physical_address;
        });
        if (unchanged == discovery_.end()) invalidate_endpoint(old.bus, old.address);
    }
    devices_ = std::move(discovery_);
    discovering_ = false;
}
Device *Hub::discovery_device(BusSide side, uint8_t address) {
    auto &devices = discovering_ ? discovery_ : devices_;
    for (auto &d : devices) if (d.bus == side && d.address == address) return &d;
    return nullptr;
}
void Hub::resolve_device(BusSide side, uint8_t address) {
    auto *found = discovery_device(side, address);
    if (!found) return;
    const auto device = *found;
    auto &devices = discovering_ ? discovery_ : devices_;
    if (device.name.empty()) { found->resolved = false; return; }
    bool ambiguous = false;
    for (auto &other : devices_) {
        if (other.name != device.name || (other.bus == side && other.address == address)) continue;
        if (other.resolved) invalidate_endpoint(other.bus, other.address);
        ambiguous = true;
    }
    const auto matches = std::count_if(devices.begin(), devices.end(), [&](const Device& other) { return other.name == device.name; });
    auto scope = std::find_if(name_scopes_.begin(), name_scopes_.end(), [&](const auto& item) { return item.first == device.name; });
    found->resolved = matches == 1 && (!ambiguous || discovering_) && (scope == name_scopes_.end() || scope->second == side);
    if (found->resolved && scope == name_scopes_.end() && configured_name(device.name)) {
        if (name_scopes_.size() < 64) name_scopes_.push_back({device.name, side});
        else found->resolved = false;
    }
    if (!discovering_ && !found->resolved) transport_.request_scan();
}
void Hub::discovered(BusSide side, uint8_t address) {
    if (address > 14 || discovery_device(side, address)) return;
    Device d; d.bus = side; d.address = address;
    if (address == 0) { d.physical_address = 0; d.type = 0; }
    (discovering_ ? discovery_ : devices_).push_back(d);
    send(side, address, {0x83});
    send(side, address, {0x46});
    send(side, address, {0x8c});
}
void Hub::identity(BusSide side, const cec::Frame &frame) {
    const auto src = frame.source(), op = frame.data[1];
    const bool front = side == BusSide::Front;
    const uint16_t pa = front ? physical_ : 0;
    if (frame.destination() == 15) return;
    switch (op) {
    case 0x83:
        if (pa != 0xffff) send(side, 15, {0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), static_cast<uint8_t>(front ? 4 : 0)});
        break;
    case 0x8c: send(side, 15, {0x87, 0, 0, 0}); break;
    case 0xa5: send(side, 15, {0xa6, 6, static_cast<uint8_t>(front ? 0x10 : 0x80), 0, 0}); break;
    case 0x46: if (src < 15) send(side, src, {0x47, 'a', 'm', 'i', 't', 'y'}); break;
    case 0x9f: if (src < 15) send(side, src, {0x9e, 6}); break;
    case 0xff: if (src < 15) send(side, src, {0, 0xff, 4}); break;
    case 0x8f: {
        if (src == 15) break;
        bool on = selected_ >= 0;
        if (on && !front) {
            auto *d = known(current().source);
            if (!d && !current().source.empty()) break; // Do not report stale/unknown equipment as off.
            on = d && d->bus == side && d->address == src;
        }
        send(side, src, {0x90, static_cast<uint8_t>(on ? 0 : 1)});
        break;
    }
    default: break; // Python is a CEC follower: unspecified opcodes are ignored.
    }
}
void Hub::receive(BusSide side, const cec::Frame &frame) {
    if (frame.size < 2 || frame.size > 16 || frame.source() == (side == BusSide::Front ? front_ : back_)) return;
    if (frame.destination() != 15 && frame.destination() != (side == BusSide::Front ? front_ : back_)) return;
    const auto op = frame.data[1];
    // The Linux CEC core enforces minimum lengths, not exact lengths.
    identity(side, frame);
    if (op == 0x84 && frame.size >= 5 && frame.destination() == 15 && frame.source() < 15) {
        discovered(side, frame.source());
        auto *d = discovery_device(side, frame.source());
        const uint16_t reported = static_cast<uint16_t>((frame.data[2] << 8) | frame.data[3]);
        for (const auto& old : devices_) if (old.resolved && old.bus == side && old.address == frame.source() &&
            old.physical_address != 0xffff && old.physical_address != reported) invalidate_endpoint(side, frame.source());
        d->physical_address = reported;
        // Only actual reports from the initial back scan count; a poll ACK or
        // a synthesized TV record is not physical-address evidence. Collect
        // before name/address deduplication can remove a responding device.
        if (discovering_ && physical_ == 0xffff && side == BusSide::Back &&
            usable_front_physical_address(d->physical_address))
            discovered_physical_ = std::min(discovered_physical_, d->physical_address);
        d->type = frame.data[4];
        resolve_device(side, frame.source());
    } else if (op == 0x47 && frame.destination() != 15) {
        if (auto *d = discovery_device(side, frame.source())) {
            // Preserve the exact valid OSD payload; malformed/missing names
            // never acquire a fabricated TV/LA-based persistent selector.
            std::string reported;
            bool valid = frame.size >= 3;
            for (uint8_t i = 2; i < frame.size; ++i) {
                if (frame.data[i] < 32 || frame.data[i] == 127) valid = false;
                reported += static_cast<char>(frame.data[i]);
            }
            if (!valid) reported.clear();
            for (const auto& old : devices_) if (old.resolved && old.bus == side && old.address == frame.source() &&
                !old.name.empty() && old.name != reported) invalidate_endpoint(side, frame.source());
            d->name = std::move(reported);
            resolve_device(side, frame.source());
        }
    } else if (op == 0x87 && frame.size >= 5 && frame.destination() == 15) {
        if (auto *d = discovery_device(side, frame.source())) d->vendor_id = (frame.data[2] << 16) | (frame.data[3] << 8) | frame.data[4];
    } else if (op == 0x7a && frame.size >= 3 && frame.destination() != 15) {
        if (auto *d = find(current().audio); d && d->bus == side && d->address == frame.source()) {
            muted_ = (frame.data[2] & 0x80) != 0;
            const auto volume = frame.data[2] & 0x7f;
            volume_ = volume <= 100 ? volume : 127;
            audio_status_known_ = true;
        }
    }
    // Source traffic is observation only. In particular, activation, routing,
    // standby and unknown vendor traffic never create a route or power grant.
}
}
