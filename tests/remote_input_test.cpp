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
enum class Key { Up, Down, Volume, Power, Shortcut, Right };
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
    explicit Fixture(ble::RemoteProfile p, uint8_t offset = 4, bool startup_complete = true)
        : profile(p), policy(output, this, offset), atomic(policy), siri(policy) {
        policy.synchronize(generation, target);
        if (startup_complete) policy.tick(ble::RemoteInputController::wait_for_release_us);
    }
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
        constexpr uint16_t usages[] = {0x52, 0x51, 0x80, 0x66, 0x3a, 0x4f};
        constexpr uint16_t bits[] = {0x200, 0x800, 2, 0x10, 8, 0x400};
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
    f.expect({{T::Press, 0x41}, {T::Release, 0x41}});
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
    f.expect({{T::Press, 1}, {T::Release, 1}});
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
    f.expect({{T::Press, 2}, {T::Release, 2}});
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
    f.expect({{T::Press, 2}, {T::Release, 2}});
    f.events.clear();
    f.key(Key::Power, true, 2000);
    f.key(Key::Power, false, 502000);
    f.expect({{T::PowerLong, 0}});
    f.events.clear();
    f.key(Key::Up, true, 503000);
    f.key(Key::Up, false, 504000);
    f.expect({{T::Press, 1}, {T::Release, 1}});
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
    siri.expect({{T::PowerShort, 0}, {T::Press, 1}, {T::Release, 1}});

    Fixture admitted(ble::RemoteProfile::Siri);
    admitted.admit_actions = true;
    admitted.key(Key::Power, true, 1000000);
    admitted.raw_siri(0x200, admitted.stamp(1100000));
    admitted.raw_siri(0, admitted.stamp(1200000));
    admitted.expect({{T::PowerShort, 0}}); // Accepted action quarantines the report's new Up.

    Fixture mixed(ble::RemoteProfile::Atomic);
    ble::ButtonEvent event{9, ble::RemoteButton::Up, true};
    mixed.policy.buttons(ble::RemoteProfile::Atomic, &event, 1, mixed.stamp(1));
    event.button = ble::RemoteButton::VolumeUp;
    mixed.policy.buttons(ble::RemoteProfile::Siri, &event, 1, mixed.stamp(2));
    event.down = false;
    mixed.policy.buttons(ble::RemoteProfile::Siri, &event, 1, mixed.stamp(3));
    mixed.expect({{T::Press, 1}, {T::Press, 0x41}, {T::Release, 0x41}});
}

static void independent_holds(ble::RemoteProfile profile) {
    Fixture f(profile);
    f.key(Key::Volume, true, 100);
    f.key(Key::Right, true, 200);
    f.key(Key::Right, false, 300);
    f.key(Key::Volume, true, 400); // Repeated state cannot restart the existing hold.
    f.expect({{T::Press, 0x41}, {T::Press, 4}, {T::Release, 4}});
    f.key(Key::Volume, false, 500);
    f.expect({{T::Press, 0x41}, {T::Press, 4}, {T::Release, 4}, {T::Release, 0x41}});

    f.events.clear();
    f.key(Key::Volume, true, 600);
    f.key(Key::Right, true, 700);
    f.cancel(); // Link/queue loss releases every independently active key.
    f.expect({{T::Press, 0x41}, {T::Press, 4}, {T::Release, 4}, {T::Release, 0x41}});

    if (profile == ble::RemoteProfile::Siri) f.raw_siri(0, f.stamp(800));
    f.key(Key::Volume, true, 900);
    f.key(Key::Right, true, 1000);
    f.events.clear();
    f.sync(2, 1); // Activity transitions release all old keys and quarantine them.
    f.expect({{T::Release, 4}, {T::Release, 0x41}});
    f.events.clear();
    f.key(Key::Right, false, 1100);
    f.key(Key::Volume, false, 1200);
    f.expect({});

    Fixture aliases(profile);
    const ble::ButtonEvent simultaneous[] = {
        {20, ble::RemoteButton::VolumeUp, true},
        {21, ble::RemoteButton::VolumeUp, true},
        {22, ble::RemoteButton::Right, true},
    };
    aliases.policy.buttons(profile, simultaneous, 3, aliases.stamp(100));
    ble::ButtonEvent release{20, ble::RemoteButton::VolumeUp, false};
    aliases.policy.buttons(profile, &release, 1, aliases.stamp(200));
    aliases.expect({{T::Press, 0x41}, {T::Press, 4}}); // One alias still holds volume.
    release = {22, ble::RemoteButton::Right, false};
    aliases.policy.buttons(profile, &release, 1, aliases.stamp(300));
    aliases.expect({{T::Press, 0x41}, {T::Press, 4}, {T::Release, 4}});
    release = {21, ble::RemoteButton::VolumeUp, false};
    aliases.policy.buttons(profile, &release, 1, aliases.stamp(400));
    aliases.expect({{T::Press, 0x41}, {T::Press, 4}, {T::Release, 4}, {T::Release, 0x41}});
}

static void wait_for_release(ble::RemoteProfile profile) {
    constexpr uint64_t second = ble::RemoteInputController::wait_for_release_us;
    Fixture startup(profile, 4, false);
    assert(startup.policy.waiting_for_release());
    startup.key(Key::Volume, true, 100);
    startup.key(Key::Right, true, 200);
    startup.key(Key::Volume, false, 300);
    assert(startup.policy.waiting_for_release()); // Right is still physically down.
    startup.key(Key::Up, true, 400); // Fresh keys are blocked too, not just old holds.
    startup.key(Key::Right, false, 500);
    assert(startup.policy.waiting_for_release());
    startup.key(Key::Up, false, 600);
    assert(!startup.policy.waiting_for_release());
    startup.expect({});
    startup.key(Key::Volume, true, 700);
    startup.key(Key::Volume, false, 800);
    startup.expect({{T::Press, 0x41}, {T::Release, 0x41}});

    Fixture timeout(profile, 4, false);
    timeout.key(Key::Volume, true, 100);
    timeout.policy.tick(second - 1);
    assert(timeout.policy.waiting_for_release());
    timeout.policy.tick(second); // No further report is needed to expire the gate.
    assert(!timeout.policy.waiting_for_release());
    timeout.key(Key::Volume, true, second + 1); // The swallowed hold cannot replay.
    timeout.key(Key::Right, true, second + 2);
    timeout.key(Key::Right, false, second + 3);
    timeout.key(Key::Volume, false, second + 4);
    timeout.expect({{T::Press, 4}, {T::Release, 4}});
    timeout.key(Key::Volume, true, second + 5);
    timeout.key(Key::Volume, false, second + 6);
    timeout.expect({{T::Press, 4}, {T::Release, 4}, {T::Press, 0x41}, {T::Release, 0x41}});

    Fixture external(profile);
    external.policy.activity_selected(0, second);
    external.key(Key::Volume, true, second + 1);
    assert(external.policy.activity_selected(1, second + 2));
    external.sync(2, 1);
    external.key(Key::Right, true, second + 3);
    external.key(Key::Volume, false, second + 4);
    assert(external.policy.waiting_for_release());
    external.key(Key::Up, true, second + 5);
    external.key(Key::Right, false, second + 6);
    external.key(Key::Up, false, second + 7);
    assert(!external.policy.waiting_for_release());
    external.expect({{T::Press, 0x41}, {T::Release, 0x41}});
    external.key(Key::Right, true, second + 8);
    external.key(Key::Right, false, second + 9);
    external.expect({{T::Press, 0x41}, {T::Release, 0x41}, {T::Press, 4}, {T::Release, 4}});

    // A same-target activity selection is a new selection even though its
    // activity generation and target stay unchanged.
    assert(external.policy.activity_selected(2, second + 10));
    external.policy.swipe(ble::RemoteButton::Right, 3, external.stamp(second + 11));
    external.key(Key::Right, true, second + 12);
    external.key(Key::Right, false, second + 13);
    assert(!external.policy.waiting_for_release());
    external.expect({{T::Press, 0x41}, {T::Release, 0x41}, {T::Press, 4}, {T::Release, 4}});
    external.policy.swipe(ble::RemoteButton::Right, 1, external.stamp(second + 14));
    assert(external.events.back().type == T::Swipe && external.events.back().count == 1);

    // Releases still choose activities from standby during the startup guard.
    Fixture standby(profile, 4, false);
    standby.sync(1, -1);
    standby.key(Key::Shortcut, true, 100);
    standby.key(Key::Shortcut, false, 200);
    standby.expect({{T::Activity, 0}});
    assert(!standby.policy.waiting_for_release());

    // Releases are applied before new presses in a report: the final old
    // release can clear the gate before a genuinely fresh button goes down.
    Fixture batch(profile, 4, false);
    ble::ButtonEvent old_down{40, ble::RemoteButton::VolumeUp, true};
    batch.policy.buttons(profile, &old_down, 1, batch.stamp(100));
    const ble::ButtonEvent changed[] = {
        {40, ble::RemoteButton::VolumeUp, false},
        {41, ble::RemoteButton::Right, true},
    };
    batch.policy.buttons(profile, changed, 2, batch.stamp(200));
    batch.expect({{T::Press, 4}});
    assert(!batch.policy.waiting_for_release());

    Fixture aliases(profile, 4, false);
    const ble::ButtonEvent alias_down[] = {
        {50, ble::RemoteButton::VolumeUp, true},
        {51, ble::RemoteButton::VolumeUp, true},
    };
    aliases.policy.buttons(profile, alias_down, 2, aliases.stamp(100));
    ble::ButtonEvent alias_up{50, ble::RemoteButton::VolumeUp, false};
    aliases.policy.buttons(profile, &alias_up, 1, aliases.stamp(200));
    assert(aliases.policy.waiting_for_release());
    alias_up.physical = 51;
    aliases.policy.buttons(profile, &alias_up, 1, aliases.stamp(300));
    assert(!aliases.policy.waiting_for_release());
    aliases.expect({});

    // As in Python, Power release is processed even if its down was swallowed.
    Fixture power(profile, 4, false);
    power.key(Key::Power, true, 100);
    power.key(Key::Power, false, 200);
    power.expect({{T::PowerShort, 0}});
}

static void selection_gate_actions(ble::RemoteProfile profile) {
    constexpr uint64_t second = ble::RemoteInputController::wait_for_release_us;
    Fixture f(profile);
    f.admit_actions = true;
    f.policy.activity_selected(0, second);
    f.key(Key::Volume, true, second + 1);
    if (profile == ble::RemoteProfile::Siri) f.key(Key::Power, true, second + 2);
    f.key(Key::Shortcut, true, second + 3);
    f.key(Key::Shortcut, false, second + 4); // Select the same activity.
    assert(f.policy.waiting_for_release());
    f.expect({{T::Press, 0x41}, {T::Activity, 0}, {T::Release, 0x41}});
    if (profile == ble::RemoteProfile::Siri) f.key(Key::Power, false, second + 5);
    f.key(Key::Right, true, second + 6);
    f.key(Key::Volume, false, second + 7);
    assert(f.policy.waiting_for_release());
    f.key(Key::Right, false, second + 8);
    assert(!f.policy.waiting_for_release());
    // Observing our own accepted request must not re-arm an already cleared gate.
    assert(!f.policy.activity_selected(1, second + 9));
    assert(!f.policy.waiting_for_release());
    f.key(Key::Right, true, second + 10);
    f.key(Key::Right, false, second + 11);
    assert(f.events.back().type == T::Release && f.events.back().value == 4);

    // If another client also selected an activity before we observe the
    // notification, only our own selection is acknowledged; the external
    // selection still arms its gate.
    Fixture coalesced(profile);
    coalesced.admit_actions = true;
    coalesced.policy.activity_selected(0, second);
    coalesced.key(Key::Power, true, second + 1);
    coalesced.key(Key::Power, false, second + 2);
    assert(!coalesced.policy.waiting_for_release());
    assert(coalesced.policy.activity_selected(2, second + 3));
    assert(coalesced.policy.waiting_for_release());

    Fixture rejected(profile);
    rejected.policy.activity_selected(0, second);
    rejected.key(Key::Volume, true, second + 1);
    if (profile == ble::RemoteProfile::Siri) rejected.key(Key::Power, true, second + 2);
    rejected.key(Key::Shortcut, true, second + 3);
    rejected.key(Key::Shortcut, false, second + 4);
    assert(!rejected.policy.waiting_for_release()); // Rejected selection cannot arm it.
    rejected.expect({{T::Press, 0x41}, {T::Activity, 0}}); // Unrelated holds continue.
    if (profile == ble::RemoteProfile::Siri) rejected.key(Key::Power, false, second + 5);
    rejected.key(Key::Right, true, second + 6);
    assert(rejected.events.back().type == T::Press && rejected.events.back().value == 4);

    Fixture repair(profile);
    repair.admit_actions = true;
    repair.policy.activity_selected(0, second);
    repair.key(Key::Power, true, second + 1);
    repair.key(Key::Power, false, second + 500001);
    repair.expect({{T::PowerLong, 0}});
    assert(!repair.policy.waiting_for_release()); // Python's fix_current_activity path.
}

static void siri_macro_defaults() {
    constexpr uint16_t selectors[] = {8, 0x200, 0x400, 0x800, 0x1000};
    constexpr int cec_keys[] = {0, 1, 4, 2, 3};
    constexpr uint16_t atomic_usages[] = {0x28, 0x52, 0x4f, 0x51, 0x50};
    for (int choice = 0; choice < 5; ++choice) {
        for (bool selector_first : {false, true}) {
            for (bool release_power_first : {false, true}) {
                for (bool long_press : {false, true}) {
                    Fixture f(ble::RemoteProfile::Siri);
                    const uint64_t start = 2000000;
                    const uint64_t selector_start = start + (selector_first ? 0 : 400000);
                    f.raw_siri(selector_first ? selectors[choice] : 0x10, f.stamp(start));
                    f.raw_siri(0x10 | selectors[choice], f.stamp(start + 400000));
                    if (selector_first) f.expect({{T::Press, cec_keys[choice]}, {T::Release, cec_keys[choice]}});
                    else f.expect({});
                    f.events.clear();
                    // Only the selector's duration matters: Power has been
                    // held much longer or much shorter in these two orders.
                    const uint64_t release = selector_start + (long_press ? 500000 : 499999);
                    f.raw_siri(release_power_first ? selectors[choice] : 0x10, f.stamp(release));
                    f.raw_siri(0, f.stamp(release + 100));
                    f.expect({{T::Activity, choice + (long_press ? 5 : 0)}});
                }
            }
        }

        Fixture atomic(ble::RemoteProfile::Atomic);
        atomic.key(Key::Power, true, 2000000);
        atomic.atomic.input(7, atomic_usages[choice], true, atomic.stamp(2000100));
        atomic.key(Key::Power, false, 2000200);
        atomic.atomic.input(7, atomic_usages[choice], false, atomic.stamp(2000300));
        atomic.expect({{T::Press, cec_keys[choice]}, {T::PowerShort, 0}, {T::Release, cec_keys[choice]}});
    }
}

static void siri_macro_lifecycle() {
    Fixture pending(ble::RemoteProfile::Siri);
    pending.key(Key::Volume, true, 2000000);
    pending.key(Key::Power, true, 2000100);
    pending.key(Key::Up, true, 2000200);
    pending.key(Key::Volume, false, 2000300); // An unrelated release cannot execute the macro.
    pending.expect({{T::Press, 0x41}, {T::Release, 0x41}});
    pending.key(Key::Up, false, 2000400);
    pending.key(Key::Power, false, 2000500);
    pending.expect({{T::Press, 0x41}, {T::Release, 0x41}, {T::Activity, 1}});

    Fixture multiple(ble::RemoteProfile::Siri);
    multiple.raw_siri(0x210, multiple.stamp(2000000)); // Power+Up chooses Up.
    multiple.raw_siri(0x218, multiple.stamp(2000100)); // A later Select cannot replace it.
    multiple.raw_siri(0x210, multiple.stamp(2000200));
    multiple.expect({{T::Press, 0}, {T::Release, 0}}); // Other selector release is ordinary input.
    multiple.raw_siri(0x10, multiple.stamp(2000300));
    multiple.raw_siri(0, multiple.stamp(2000400));
    multiple.expect({{T::Press, 0}, {T::Release, 0}, {T::Activity, 1}});

    Fixture rejected(ble::RemoteProfile::Siri);
    rejected.key(Key::Volume, true, 2000000);
    rejected.key(Key::Power, true, 2000100);
    rejected.key(Key::Up, true, 2000200);
    rejected.key(Key::Up, false, 2000300);
    rejected.key(Key::Power, false, 2000400);
    rejected.expect({{T::Press, 0x41}, {T::Activity, 1}});
    assert(!rejected.policy.waiting_for_release());
    // The macro stays executed until every physical key is up, even when
    // selection was rejected. A new chord cannot become another macro or a
    // standalone Power action while Volume remains physically held.
    rejected.key(Key::Power, true, 2000500);
    rejected.key(Key::Right, true, 2000600);
    rejected.key(Key::Right, false, 2000700);
    rejected.key(Key::Power, false, 2000800);
    rejected.expect({{T::Press, 0x41}, {T::Activity, 1}, {T::Press, 4}, {T::Release, 4}});
    rejected.key(Key::Volume, false, 2000900);
    rejected.key(Key::Power, true, 2001000);
    rejected.key(Key::Right, true, 2001100);
    rejected.key(Key::Right, false, 2001200);
    rejected.key(Key::Power, false, 2001300);
    rejected.expect({{T::Press, 0x41}, {T::Activity, 1}, {T::Press, 4}, {T::Release, 4},
                     {T::Release, 0x41}, {T::Activity, 2}});

    Fixture accepted(ble::RemoteProfile::Siri);
    accepted.admit_actions = true;
    accepted.key(Key::Volume, true, 2000000);
    accepted.key(Key::Power, true, 2000100);
    accepted.key(Key::Shortcut, true, 2000200);
    accepted.key(Key::Shortcut, false, 2000300); // Same-target selection leaves the activity active.
    accepted.key(Key::Power, false, 2000400);
    accepted.expect({{T::Press, 0x41}, {T::Activity, 0}, {T::Release, 0x41}});
    assert(accepted.policy.waiting_for_release());
    accepted.policy.tick(3000300);
    assert(!accepted.policy.waiting_for_release());
    // The one-second wait fallback does not reset the executed macro latch.
    accepted.key(Key::Power, true, 3000400);
    accepted.key(Key::Up, true, 3000500);
    accepted.key(Key::Up, false, 3000600);
    accepted.key(Key::Power, false, 3000700);
    accepted.expect({{T::Press, 0x41}, {T::Activity, 0}, {T::Release, 0x41},
                     {T::Press, 1}, {T::Release, 1}});
    accepted.key(Key::Volume, false, 3000800);
    accepted.key(Key::Power, true, 3000900);
    accepted.key(Key::Shortcut, true, 3001000);
    accepted.key(Key::Shortcut, false, 3001100);
    accepted.key(Key::Power, false, 3001200);
    accepted.expect({{T::Press, 0x41}, {T::Activity, 0}, {T::Release, 0x41},
                     {T::Press, 1}, {T::Release, 1}, {T::Activity, 0}});
}

int main() {
    for (auto profile : {ble::RemoteProfile::Atomic, ble::RemoteProfile::Siri}) {
        activity_quarantine(profile);
        queued_reports(profile);
        cancellation(profile);
        admissions_and_repairs(profile);
        independent_holds(profile);
        wait_for_release(profile);
        selection_gate_actions(profile);
    }
    profiles_and_batches();
    touch_generations();
    siri_macro_defaults();
    siri_macro_lifecycle();
    puts("Shared remote policy: activity/stale-report quarantine, cancellation, admission, repairs and profiles passed");
}
