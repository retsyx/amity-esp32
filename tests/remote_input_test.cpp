/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "atomic_controller.hpp"
#include "siri_remote.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

using T = ble::RemoteCommandType;
using Pair = std::pair<T, int>;
enum class Key { Up, Down, Volume, Power, Shortcut };
struct Fixture {
    ble::RemoteProfile profile;
    std::vector<ble::RemoteCommand> events;
    bool admit_actions = false;
    uint32_t generation = 1;
    int target = 0;
    uint16_t mask = 0;
    ble::RemoteInputController policy;
    ble::AtomicInputAdapter atomic;
    ble::SiriInputAdapter siri;
    explicit Fixture(ble::RemoteProfile p, uint8_t offset = 4)
        : profile(p), policy(output, this, offset), atomic(policy), siri(policy) { policy.synchronize(generation, target); }
    static bool output(ble::RemoteCommand command, void *context) {
        auto &self = *static_cast<Fixture *>(context);
        self.events.push_back(command);
        return command.type == T::Press || command.type == T::Release || command.type == T::Swipe || self.admit_actions;
    }
    ble::InputStamp stamp(uint64_t now) const { return {now, generation, target}; }
    void sync(uint32_t next_generation, int next_target) {
        generation = next_generation; target = next_target;
        if (policy.synchronize(generation, target)) siri.activity_changed();
    }
    void raw_siri(uint16_t next, ble::InputStamp at) {
        mask = next;
        const uint8_t data[] = {static_cast<uint8_t>(next), static_cast<uint8_t>(next >> 8)};
        siri.buttons(data, sizeof(data), at);
    }
    void key_at(Key key, bool down, ble::InputStamp at) {
        constexpr uint16_t usages[] = {0x52, 0x51, 0x80, 0x66, 0x3a};
        constexpr uint16_t bits[] = {0x200, 0x800, 2, 0x10, 8};
        const auto i = static_cast<size_t>(key);
        if (profile == ble::RemoteProfile::Atomic) atomic.input(7, usages[i], down, at);
        else raw_siri(down ? mask | bits[i] : mask & ~bits[i], at);
    }
    void key(Key key, bool down, uint64_t now) { key_at(key, down, stamp(now)); }
    void cancel() { policy.cancel(); siri.cancel(); }
    void expect(std::initializer_list<Pair> expected) const {
        if (events.size() != expected.size()) {
            std::fprintf(stderr,"profile=%d expected=%zu actual=%zu\n",static_cast<int>(profile),expected.size(),events.size());
            for (const auto &event : events) std::fprintf(stderr,"  type=%d value=%d\n",static_cast<int>(event.type),event.value);
        }
        assert(events.size() == expected.size());
        size_t i = 0;
        for (const auto &event : expected) {
            assert(events[i].type == event.first && events[i].value == event.second);
            ++i;
        }
    }
};

static void activity_quarantine(ble::RemoteProfile profile) {
    Fixture f(profile);
    f.key(Key::Up, true, 100);
    f.key(Key::Volume, true, 200);
    f.sync(2, 1); // Web/Matter/HomeKit selects another active source.
    f.events.clear();
    f.key(Key::Volume, false, 300);
    f.key(Key::Up, false, 400);
    f.expect({}); // Releasing volume cannot restart the old arrow on source 1.
    f.key(Key::Up, true, 500);
    f.expect({{T::Press, 1}});
    assert(f.events.back().stamp.generation == 2 && f.events.back().stamp.target == 1);
    f.sync(3, -1); // Standby cancels this hold too.
    f.sync(4, 1);
    f.events.clear();
    f.key(Key::Up, false, 600);
    f.expect({});
    f.key(Key::Up, true, 700);
    f.expect({{T::Press, 1}});
    f.sync(5, 1); // Configuration replacement / same-index generation change.
    f.events.clear();
    f.key(Key::Up, false, 800);
    f.expect({});

    for (Key pending : {Key::Power, Key::Shortcut}) {
        f.key(pending, true, 1000);
        f.sync(++f.generation, 2);
        f.events.clear();
        f.key(pending, false, 900000);
        f.expect({}); // Neither a long power hold nor selector can complete on a new activity.
    }
}

static void queued_reports(ble::RemoteProfile profile) {
    Fixture f(profile);
    const auto old = f.stamp(100);
    f.sync(2, 1);
    f.key_at(Key::Up, true, old); // Queued before the transition, consumed afterward.
    f.key(Key::Up, true, 200); // A repeated full state/delta cannot rearm it.
    f.key(Key::Up, false, 300);
    f.expect({});
    f.key(Key::Up, true, 400);
    f.expect({{T::Press, 1}});
    f.events.clear();
    const ble::ButtonEvent stale_release{
        profile == ble::RemoteProfile::Atomic ? 0x00070052u : 9u, ble::RemoteButton::Up, false};
    // Even a delayed normalized release cannot stop a newer held key.
    f.policy.buttons(profile, &stale_release, 1, {500, 1, 0});
    f.expect({});
    f.key(Key::Volume, true, 600);
    f.key(Key::Volume, false, 700);
    f.expect({{T::Release, 0}, {T::Press, 0x41}, {T::Release, 0}, {T::Press, 1}});
    f.key(Key::Up, false, 800);
    f.events.clear();
    f.sync(2, 2); // Reserved target advances before the published generation.
    f.key_at(Key::Power, true, {900, 2, 1});
    f.key(Key::Power, false, 1000000);
    f.expect({});
}

static void cancellation(ble::RemoteProfile profile) {
    Fixture f(profile);
    f.key(Key::Up, true, 100);
    f.key(Key::Power, true, 200);
    f.cancel();
    f.events.clear();
    // Offline / dropped release: HID emits no further edge for the old hold;
    // Siri's next full empty mask must release its retained physical mask.
    if (profile == ble::RemoteProfile::Siri) f.raw_siri(0, f.stamp(300));
    f.policy.swipe(ble::RemoteButton::Up, 1, f.stamp(400));
    f.expect({{T::Swipe, 1}}); // No ghost key may block future touch input.
    f.events.clear();
    f.key(Key::Up, true, 500); // First genuine new press recovers after a dropped up.
    f.key(Key::Up, false, 600);
    f.expect({{T::Press, 1}, {T::Release, 0}});
    f.events.clear();
    f.key(Key::Power, false, 1000000);
    f.expect({}); // No pending power action survived cancellation.
    if (profile == ble::RemoteProfile::Siri) {
        f.key(Key::Up, true, 1100000);
        f.cancel();
        f.events.clear();
        f.key(Key::Up, true, 1200000); // Unchanged full-state report cannot replay after loss.
        f.key(Key::Up, false, 1300000);
        f.expect({});
        f.key(Key::Up, true, 1400000);
        f.expect({{T::Press, 1}});
    }
}

static void admissions_and_repairs(ble::RemoteProfile profile) {
    Fixture f(profile);
    f.admit_actions = true;
    f.sync(1, 1);
    // Atomic F1 / Siri Power+Select both select activity zero.
    if (profile == ble::RemoteProfile::Siri) f.key(Key::Power, true, 100);
    f.key(Key::Shortcut, true, 200);
    f.key(Key::Shortcut, false, 300);
    if (profile == ble::RemoteProfile::Siri) f.key(Key::Power, false, 400);
    f.expect({{T::Activity, 0}});
    f.key(Key::Up, true, 500); // Admitted transition, reservation not published yet.
    f.expect({{T::Activity, 0}});
    f.sync(1, 0); // Reservation, then publication, suppress every pre-transition hold.
    f.sync(2, 0);
    f.key(Key::Up, false, 600);
    f.events.clear();
    f.key(Key::Down, true, 700);
    f.key(Key::Down, false, 800);
    f.expect({{T::Press, 2}, {T::Release, 0}});
    f.events.clear();
    // Same-target repairs do not necessarily publish a new generation.
    if (profile == ble::RemoteProfile::Siri) f.key(Key::Power, true, 1000);
    f.key(Key::Shortcut, true, 1100);
    f.key(Key::Shortcut, false, 1200);
    if (profile == ble::RemoteProfile::Siri) f.key(Key::Power, false, 1300);
    f.expect({{T::Activity, 0}});
    f.events.clear();
    f.key(Key::Down, true, 1400);
    f.key(Key::Down, false, 1500);
    f.expect({{T::Press, 2}, {T::Release, 0}});
    f.events.clear();
    f.key(Key::Power, true, 2000);
    f.key(Key::Power, false, 502000);
    f.expect({{T::PowerLong, 0}});
    f.events.clear();
    f.key(Key::Up, true, 503000);
    f.key(Key::Up, false, 504000);
    f.expect({{T::Press, 1}, {T::Release, 0}});
}

static void touch_report(Fixture &f, uint16_t remote_time, int x, uint8_t pressure, ble::InputStamp stamp) {
    std::array<uint8_t, 11> report{};
    report[0] = 0x32;
    report[1] = static_cast<uint8_t>(remote_time);
    report[2] = static_cast<uint8_t>(remote_time >> 8);
    const unsigned packed_x = static_cast<unsigned>(x - 360) & 0xfff;
    const unsigned packed_y = static_cast<unsigned>(-360) & 0xfff;
    report[4] = static_cast<uint8_t>(packed_x);
    report[5] = static_cast<uint8_t>((packed_x >> 8) | ((packed_y & 15) << 4));
    report[6] = static_cast<uint8_t>(packed_y >> 4);
    report[9] = pressure;
    report[10] = 8;
    f.siri.touches(report.data(), report.size(), stamp);
}

static void touch_generations() {
    Fixture f(ble::RemoteProfile::Siri);
    const auto old = f.stamp(100);
    f.sync(2, 1);
    touch_report(f, 100, 0, 30, old); // Old queued finger begins after the switch.
    touch_report(f, 200, 620, 30, f.stamp(200));
    touch_report(f, 300, 620, 0, f.stamp(300));
    f.expect({}); // Quarantine lasts through touch-up.
    touch_report(f, 400, 0, 30, f.stamp(400));
    touch_report(f, 500, 620, 30, f.stamp(500));
    f.expect({{T::Swipe, 4}});
    assert(f.events.back().count == 2 && f.events.back().stamp.generation == 2);
    f.sync(3, 2); // A live swipe also cannot continue on another active source.
    f.events.clear();
    touch_report(f, 600, 1240, 30, f.stamp(600));
    touch_report(f, 700, 1240, 0, f.stamp(700));
    f.expect({});
}

static void profiles_and_batches() {
    Fixture atomic(ble::RemoteProfile::Atomic, 5);
    atomic.key(Key::Shortcut, true, 100);
    atomic.key(Key::Shortcut, false, 500100);
    atomic.expect({{T::Activity, 5}}); // Configurable Atomic long offset is preserved.
    Fixture siri(ble::RemoteProfile::Siri);
    siri.raw_siri(0x210, siri.stamp(100)); // Simultaneous Up+Power emits no stray navigation press.
    siri.expect({});
    siri.raw_siri(0, siri.stamp(500100)); // Selector release precedes Power in the same batch.
    siri.expect({{T::Activity, 6}});
    siri.events.clear();
    siri.key(Key::Power, true, 1000000);
    siri.raw_siri(0x200, siri.stamp(1100000)); // New Up shares a Power release report.
    siri.raw_siri(0, siri.stamp(1200000));
    siri.expect({{T::PowerShort, 0}});

    Fixture mixed(ble::RemoteProfile::Atomic);
    ble::ButtonEvent event{9, ble::RemoteButton::Up, true};
    mixed.policy.buttons(ble::RemoteProfile::Atomic, &event, 1, mixed.stamp(1));
    event.button = ble::RemoteButton::VolumeUp;
    mixed.policy.buttons(ble::RemoteProfile::Siri, &event, 1, mixed.stamp(2));
    event.down = false;
    mixed.policy.buttons(ble::RemoteProfile::Siri, &event, 1, mixed.stamp(3));
    mixed.expect({{T::Press, 1}, {T::Release, 0}, {T::Press, 0x41}, {T::Release, 0}, {T::Press, 1}});
}

int main() {
    for (auto profile : {ble::RemoteProfile::Atomic, ble::RemoteProfile::Siri}) {
        activity_quarantine(profile);
        queued_reports(profile);
        cancellation(profile);
        admissions_and_repairs(profile);
    }
    profiles_and_batches();
    touch_generations();
    puts("Shared remote policy: activity/stale-report quarantine, cancellation, admission, repairs and profiles passed");
}
