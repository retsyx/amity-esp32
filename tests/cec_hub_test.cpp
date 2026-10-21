/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "cec/engine.hpp"
#include "amity/hub.hpp"
#include "volume_sequence.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static cec::Frame frame(std::initializer_list<uint8_t> bytes) {
    cec::Frame f; for (const auto b : bytes) f.data[f.size++] = b; return f;
}
static bool same(const cec::Frame &a, const cec::Frame &b) {
    return a.size == b.size && !std::memcmp(a.data, b.data, a.size);
}
struct Wire {
    cec::Engine a, b, c;
    bool al = false, bl = false, cl = false;
    uint64_t now = 0;
    void tick() {
        const bool high = !(al || bl || cl);
        al = a.tick(now, high); bl = b.tick(now, high); cl = c.tick(now, high);
        now += 50;
    }
    void run(uint64_t usec) { const auto end = now + usec; while (now < end) tick(); }
};
static void wire_tests() {
    { // Directed multi-byte traffic, recipient ACK, no sender self-delivery.
        Wire w; w.a.set_address(4); w.b.set_address(0);
        const auto f = frame({0x40, 0x44, 0x6d});
        assert(w.a.begin(f, 0)); w.run(150000);
        assert(w.a.take_result() == cec::TxResult::Success);
        cec::Frame rx; assert(w.b.take_received(rx)); assert(same(rx, f)); assert(!w.a.take_received(rx));
    }
    { // Broadcast ACK is inverted: receivers leave the slot high.
        Wire w; w.a.set_address(4); w.b.set_address(0); w.c.set_address(5);
        const auto f = frame({0x4f, 0x82, 0x10, 0});
        assert(w.a.begin(f, 0)); w.run(160000);
        assert(w.a.take_result() == cec::TxResult::Success);
        cec::Frame rx; assert(w.b.take_received(rx) && same(rx, f)); assert(w.c.take_received(rx) && same(rx, f));
    }
    { // Polling an absent address must NACK, never claim success.
        Wire w; w.a.set_address(4); w.b.set_address(0);
        assert(w.a.begin(frame({0x48}), 0)); w.run(80000);
        assert(w.a.take_result() == cec::TxResult::Nack);
    }
    { // Simultaneous initiators arbitrate on their header; losing node releases.
        Wire w; w.a.set_address(4); w.b.set_address(8); w.c.set_address(0);
        const auto winner = frame({0x40, 0x8f});
        assert(w.a.begin(winner, 0)); assert(w.b.begin(frame({0x80, 0x46}), 0));
        w.run(130000);
        assert(w.a.take_result() == cec::TxResult::Success);
        assert(w.b.take_result() == cec::TxResult::ArbitrationLost);
        cec::Frame rx; assert(w.c.take_received(rx) && same(rx, winner));
    }
    { // Full 16-byte frame exercises length limit and all data bits.
        Wire w; w.a.set_address(4); w.b.set_address(0);
        auto f = frame({0x40, 0x47, 0x55, 0xaa, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        assert(w.a.begin(f, 0)); w.run(450000);
        assert(w.a.take_result() == cec::TxResult::Success);
        cec::Frame rx; assert(w.b.take_received(rx) && same(rx, f));
    }
    { // Bus held low cannot result in success or an endless transmission.
        cec::Engine e; assert(e.begin(frame({0x4f, 0x36}), 0));
        for (uint64_t t = 0; t < 1100000; t += 50) e.tick(t, false);
        assert(e.take_result() == cec::TxResult::Timeout);
    }
    { // A missed timer deadline aborts the wire operation instead of glitching.
        Wire w; assert(w.a.begin(frame({0x4f, 0x36}), 0)); w.run(19000);
        w.now += 1000; w.tick();
        assert(w.a.take_result() == cec::TxResult::LineError);
    }
    { cec::Engine e; cec::Frame f; assert(!e.begin(f, 0)); f.size = 17; assert(!e.begin(f, 0)); }
}

struct Transport : amity::Transport {
    struct Sent { amity::BusSide side; uint8_t destination; std::vector<uint8_t> bytes; };
    std::vector<Sent> sent;
    unsigned scans = 0;
    int reject_opcode = -1;
    bool send(amity::BusSide side, uint8_t dest, const uint8_t *data, size_t size) override {
        if (size && data[0] == reject_opcode) return false;
        sent.push_back({side, dest, {data, data + size}}); return true;
    }
    void request_scan() override { ++scans; }
    bool contains(amity::BusSide side, uint8_t dest, std::initializer_list<uint8_t> bytes) const {
        for (const auto &s : sent) if (s.side == side && s.destination == dest && s.bytes == std::vector<uint8_t>(bytes)) return true;
        return false;
    }
    unsigned count(amity::BusSide side, uint8_t dest, std::initializer_list<uint8_t> bytes) const {
        unsigned matches = 0;
        for (const auto &s : sent) if (s.side == side && s.destination == dest && s.bytes == std::vector<uint8_t>(bytes)) ++matches;
        return matches;
    }
};
static void device(amity::Hub &hub, amity::BusSide side, uint8_t address, uint16_t pa, uint8_t type, const char *name) {
    hub.discovered(side, address);
    hub.receive(side, frame({static_cast<uint8_t>((address << 4) | 15), 0x84, static_cast<uint8_t>(pa >> 8), static_cast<uint8_t>(pa), type}));
    auto f = frame({static_cast<uint8_t>((address << 4) | (side == amity::BusSide::Front ? 4 : 0)), 0x47});
    for (const char *p = name; *p && f.size < 16; ++p) f.data[f.size++] = *p;
    hub.receive(side, f);
}
static void hub_tests() {
    using amity::BusSide;
    const auto F = BusSide::Front, B = BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    h.announce(F);
    assert(t.sent.size() == 3);
    assert((t.sent[0].bytes == std::vector<uint8_t>{0xa6, 6, 0x10, 0, 0}));
    assert((t.sent[1].bytes == std::vector<uint8_t>{0x84, 0x10, 0, 4}));
    assert((t.sent[2].bytes == std::vector<uint8_t>{0x87, 0, 0, 0}));
    h.configure({{"TV", "Screen", "Apple TV", "AVR", "", 0}, {"Game", "Screen", "PS5", "AVR", "", 0},
                 {"Analog", "Screen", "", "AVR", "AVR", 3}});
    device(h, F, 0, 0, 0, "Screen"); device(h, B, 4, 0x1500, 4, "Apple TV");
    device(h, B, 11, 0x1400, 4, "PS5"); device(h, B, 5, 0x1000, 5, "AVR");
    t.sent.clear();
    assert(!h.select_activity(-2)); assert(!h.select_activity(3));
    assert(h.select_activity(0)); assert(h.snapshot().current_activity == 0);
    assert(t.contains(F, 0, {0x04})); assert(t.contains(B, 4, {0x44, 0x6d}));
    assert(t.contains(B, 5, {0x44, 0x6d})); assert(t.contains(B, 15, {0x86, 0x15, 0}));
    t.sent.clear(); h.select_activity(1);
    assert(t.contains(B, 4, {0x36})); assert(t.contains(B, 11, {0x44, 0x6d}));
    assert(!t.contains(F, 0, {0x36})); assert(!t.contains(B, 5, {0x36}));
    t.sent.clear(); h.select_activity(1); assert(t.contains(F, 0, {0x04}));
    t.sent.clear(); assert(h.press_key(0x41, true)); h.release_key();
    assert(t.contains(B, 5, {0x44, 0x41})); assert(t.contains(B, 5, {0x45}));
    assert(!t.contains(B, 5, {0x71})); h.tick(500000);
    assert(t.contains(B, 5, {0x71}));
    h.receive(B, frame({0x50, 0x7a, 0x80 | 42}));
    assert(h.snapshot().audio_status_known && h.snapshot().muted && h.snapshot().volume == 42);
    h.press_key(0x66); assert(t.contains(B, 5, {0x44, 0x66}));
    assert(!h.snapshot().audio_status_known);
    h.receive(B, frame({0x50, 0x7a, 127}));
    assert(h.snapshot().audio_status_known && !h.snapshot().muted && h.snapshot().volume == 127);
    assert(h.press_key(0)); assert(t.contains(B, 11, {0x44, 0}));
    t.sent.clear(); h.press_key(0x61, true); h.press_key(0x61, true); h.release_key();
    assert(t.contains(B, 11, {0x44, 0x44})); assert(!t.contains(B, 11, {0x44, 0x46}));
    h.press_key(0x61); assert(t.contains(B, 11, {0x44, 0x46}));
    t.sent.clear(); h.receive(F, frame({0x04, 0x8f})); assert(t.contains(F, 0, {0x90, 0}));
    h.receive(B, frame({0x40, 0x8f})); assert(t.contains(B, 4, {0x90, 1}));
    h.receive(B, frame({0xb0, 0x8f})); assert(t.contains(B, 11, {0x90, 0}));
    t.sent.clear(); h.receive(B, frame({0x4f, 0x82, 0x15, 0}));
    assert(t.contains(B, 4, {0x36})); assert(t.contains(B, 15, {0x86, 0x14, 0}));
    t.sent.clear(); h.receive(B, frame({0xbf, 0x82, 0x14, 0})); assert(t.sent.empty());
    h.receive(B, frame({0xbf, 0x84, 0x13, 0, 4})); assert(t.contains(B, 15, {0x86, 0x13, 0}));
    t.sent.clear(); h.select_activity(2);
    assert(t.contains(B, 5, {0x44, 0x69, 3})); assert(t.contains(F, 15, {0x82, 0x10, 0}));
    h.standby(); assert(h.snapshot().current_activity == -1);
    t.sent.clear(); h.standby(); assert(t.contains(F, 15, {0x36}) && t.contains(B, 15, {0x36}));
    t.sent.clear(); h.receive(B, frame({0x4f, 0x82, 0x15, 0}));
    assert(t.contains(F, 15, {0x36}) && t.contains(B, 15, {0x36}));
    assert(!h.press_key(0));
    t.sent.clear(); h.receive(F, frame({0x04, 0x83})); assert(t.contains(F, 15, {0x84, 0x10, 0, 4}));
    h.receive(B, frame({0x40, 0x83})); assert(t.contains(B, 15, {0x84, 0, 0, 0}));
    h.receive(F, frame({0x04, 0x9f})); assert(t.contains(F, 0, {0x9e, 6}));
    h.receive(B, frame({0x40, 0xa5})); assert(t.contains(B, 15, {0xa6, 6, 0x80, 0, 0}));
    h.receive(F, frame({0x04, 0x46})); assert(t.contains(F, 0, {0x47, 'a', 'm', 'i', 't', 'y'}));
    t.sent.clear(); h.receive(F, frame({0x0f, 0x83})); assert(t.sent.empty());
    h.receive(F, frame({0xf4, 0x8f})); assert(t.sent.empty());
    h.receive(F, frame({0x04, 0xfe})); assert(t.sent.empty()); // Unknown op: follower suppresses automatic abort.
    h.receive(F, frame({0x04, 0, 0xfe, 0})); assert(t.sent.empty()); // Never abort a Feature Abort.
    h.receive(B, frame({0x4f, 0x04})); assert(t.sent.empty()); // Image View On must be directed.
    h.receive(B, frame({0x40, 0x82, 0x15, 0})); assert(t.sent.empty()); // Active Source must broadcast.
    h.receive(F, frame({0x04, 0xff})); assert(t.contains(F, 0, {0, 0xff, 4}));
    t.sent.clear(); h.receive(F, frame({0x04, 0x9f, 0})); assert(t.contains(F, 0, {0x9e, 6}));
    // Vendor power quirks apply to activity entry/full standby, not switching.
    h.receive(B, frame({0x4f, 0x87, 0, 0x12, 0x34}));
    h.configure({{"TV", "Screen", "Apple TV", "AVR", "", 0}}, {{0x1234, {0x44, 0x40}, {0x36}}});
    t.sent.clear(); h.select_activity(0); assert(t.contains(B, 4, {0x44, 0x40})); assert(t.contains(B, 4, {0x45}));
    // Device matching is scoped to a bus: front TV must survive a back PA=0.
    device(h, B, 8, 0, 4, "Duplicate PA");
    bool found_tv = false; for (const auto &d : h.snapshot().devices) if (d.name == "Screen") found_tv = true;
    assert(found_tv);
}
static void discovery_tests() {
    const auto F = amity::BusSide::Front, B = amity::BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    h.configure({{"Watch", "Screen", "Source", "Source", "", 0}});
    assert(h.activity_needs_discovery(0));
    device(h, F, 0, 0, 0, "Screen"); device(h, B, 4, 0x1000, 4, "Source");
    assert(!h.activity_needs_discovery(0));
    h.select_activity(0); assert(!h.key_needs_discovery(0));
    // Discovery leaves the working cache and selected-source power response
    // intact until all metadata has been collected into its replacement.
    h.begin_discovery();
    assert(h.snapshot().devices.size() == 2);
    t.sent.clear(); h.receive(B, frame({0x40, 0x8f}));
    assert(t.contains(B, 4, {0x90, 0}));
    device(h, F, 0, 0, 0, "Screen"); device(h, B, 8, 0x2000, 4, "Source");
    t.sent.clear(); h.receive(B, frame({0x40, 0x8f}));
    assert(t.contains(B, 4, {0x90, 0}));
    assert(!t.contains(B, 15, {0x86, 0x20, 0}));
    h.finish_discovery();
    assert(t.contains(B, 15, {0x86, 0x20, 0}));
    assert(h.snapshot().devices.size() == 2);
    t.sent.clear(); h.press_key(0); assert(t.contains(B, 8, {0x44, 0}));
    // A selected source reconnects at another logical and physical address.
    // Its late OSD reply must replace the old name and restore the new route.
    t.sent.clear();
    h.receive(B, frame({0xbf, 0x84, 0x30, 0, 4}));
    assert(!t.contains(B, 15, {0x86, 0x30, 0}));
    h.receive(B, frame({0xb0, 0x47, 'S', 'o', 'u', 'r', 'c', 'e'}));
    assert(t.contains(B, 15, {0x86, 0x30, 0}));
    assert(h.snapshot().devices.size() == 2);
    t.sent.clear(); h.press_key(0); assert(t.contains(B, 11, {0x44, 0}));
    // The opposite metadata reply order must never route to unknown PA ffff.
    h.discovered(B, 4);
    t.sent.clear(); h.receive(B, frame({0x40, 0x47, 'S', 'o', 'u', 'r', 'c', 'e'}));
    assert(!t.contains(B, 15, {0x86, 0xff, 0xff}));
    h.receive(B, frame({0x4f, 0x84, 0x40, 0, 4}));
    assert(t.contains(B, 15, {0x86, 0x40, 0}));
    assert(h.snapshot().devices.size() == 2);
    h.clear_devices(); assert(h.key_needs_discovery(0));
}
static void shared_power_tests() {
    const auto F = amity::BusSide::Front, B = amity::BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    h.configure({{"Shared", "Front", "Back", "Back", "Back", 0},
                 {"Other", "", "Other", "Other", "", 0}},
                {{0x1234, {0x44, 0x40}, {0x44, 0x40}}});
    device(h, F, 5, 0x1000, 5, "Front"); device(h, B, 5, 0x1000, 5, "Back");
    device(h, B, 8, 0x2000, 4, "Other");
    for (const auto header : {0x5f, 0x8f}) h.receive(B, frame({static_cast<uint8_t>(header), 0x87, 0, 0x12, 0x34}));
    h.receive(F, frame({0x5f, 0x87, 0, 0x12, 0x34}));
    // Reused source/audio/switch roles send one toggle, while the same logical
    // address on another bus remains a separate device. Repairs do the same.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        t.sent.clear(); assert(h.select_activity(0));
        assert(t.count(F, 5, {0x44, 0x40}) == 1);
        assert(t.count(B, 5, {0x44, 0x40}) == 1);
        assert(t.count(B, 8, {0x44, 0x40}) == 0);
    }
    t.sent.clear(); h.standby();
    assert(t.count(F, 5, {0x44, 0x40}) == 1);
    assert(t.count(B, 5, {0x44, 0x40}) == 1);
    assert(h.select_activity(0)); t.sent.clear(); assert(h.select_activity(1));
    assert(t.count(F, 5, {0x36}) == 1);
    assert(t.count(B, 5, {0x36}) == 1);
    assert(t.count(B, 8, {0x44, 0x40}) == 1);
    t.sent.clear(); h.standby(); assert(t.count(B, 8, {0x44, 0x40}) == 1);
}
static void playback_tests() {
    const auto B = amity::BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    h.configure({{"Watch", "", "Source", "", "", 0},
                 {"Other", "", "Source B", "", "", 0},
                 {"Same source", "", "Source B", "", "", 0}});
    device(h, B, 4, 0x1000, 4, "Source"); assert(h.select_activity(0));
    device(h, B, 8, 0x2000, 4, "Source B");
    const auto check = [&](std::initializer_list<uint8_t> keys, std::initializer_list<uint8_t> expected) {
        t.sent.clear();
        for (const auto key : keys) assert(h.press_key(key));
        std::vector<uint8_t> emitted;
        for (const auto &s : t.sent) if (s.side == B && s.destination == 4 && s.bytes.size() == 2 && s.bytes[0] == 0x44)
            emitted.push_back(s.bytes[1]);
        assert(emitted == std::vector<uint8_t>(expected));
    };
    // Explicit commands from Matter/HomeKit and an emulated remote toggle
    // share the last accepted playback state.
    check({0x44, 0x61}, {0x44, 0x46});
    check({0x61, 0x46, 0x61}, {0x44, 0x46, 0x44});
    check({0x44, 0x45, 0x61}, {0x44, 0x45, 0x44});
    // Holding the toggle repeats its initial command without toggling again.
    assert(h.press_key(0x61, true)); assert(h.press_key(0x61, true)); assert(h.release_key());
    check({0x61}, {0x44});
    // Rejected explicit Pause/Stop cannot change a previously accepted Play.
    for (const auto key : {0x46, 0x45}) {
        assert(h.press_key(0x44)); t.reject_opcode = 0x44; assert(!h.press_key(key));
        t.reject_opcode = -1; check({0x61}, {0x46});
    }
    // Rejected explicit Play likewise cannot turn a paused state into playing.
    t.reject_opcode = 0x44; assert(!h.press_key(0x44));
    t.reject_opcode = -1; check({0x61}, {0x44});
    // Playback intent belongs to the selected source, so a new source starts
    // with Play. Activities sharing that source retain the accepted state.
    assert(h.select_activity(1)); t.sent.clear(); assert(h.press_key(0x61));
    assert(t.contains(B, 8, {0x44, 0x44}) && !t.contains(B, 8, {0x44, 0x46}));
    assert(h.select_activity(2)); t.sent.clear(); assert(h.press_key(0x61));
    assert(t.contains(B, 8, {0x44, 0x46}) && !t.contains(B, 8, {0x44, 0x44}));
}
static void backpressure_tests() {
    const auto B = amity::BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    h.configure({{"Listen", "", "Source", "Source", "", 0}});
    device(h, B, 4, 0x1000, 4, "Source"); h.select_activity(0);
    // A final release is attempted once, using the shared driver FIFO. If
    // that slot is full, no future up, tick or new key retries the dropped TX.
    t.sent.clear(); t.reject_opcode = 0x45;
    assert(h.press_key(0)); assert(t.contains(B, 4, {0x44, 0}));
    assert(!t.contains(B, 4, {0x45}));
    assert(h.press_key(1)); assert(t.contains(B, 4, {0x44, 1}));
    h.tick(10000); assert(!t.contains(B, 4, {0x45}));
    t.reject_opcode = -1; h.tick(20000); assert(!t.contains(B, 4, {0x45}));
    assert(h.press_key(1)); assert(t.contains(B, 4, {0x44, 1}));
    // Failed initial presses must not toggle the emulated playback state.
    t.sent.clear(); t.reject_opcode = 0x44; assert(!h.press_key(0x61));
    t.reject_opcode = -1; assert(h.press_key(0x61));
    assert(t.contains(B, 4, {0x44, 0x44})); assert(!t.contains(B, 4, {0x44, 0x46}));
    // Power-on and switch-input releases are likewise never buffered/retried.
    t.sent.clear(); t.reject_opcode = 0x45; h.fix_current_activity();
    assert(t.contains(B, 4, {0x44, 0x6d})); assert(!t.contains(B, 4, {0x45}));
    t.reject_opcode = -1; h.tick(30000); assert(!t.contains(B, 4, {0x45}));
    t.sent.clear();t.reject_opcode=0x44;h.fix_current_activity();assert(!t.contains(B,4,{0x45}));t.reject_opcode=-1;
    // One long volume adjustment is 100 complete presses, but only one
    // status query after the last press has been quiet for half a second.
    t.sent.clear();
    for (uint64_t i = 1; i <= 100; ++i) { h.tick(i * 250000); assert(h.press_key(0x41)); }
    assert(!t.contains(B, 4, {0x71}));
    h.tick(25499999); assert(!t.contains(B, 4, {0x71}));
    h.tick(25500000); assert(t.contains(B, 4, {0x71}));
    unsigned presses = 0, releases = 0, queries = 0;
    for (const auto &sent : t.sent) {
        if (sent.bytes == std::vector<uint8_t>{0x44, 0x41}) ++presses;
        if (sent.bytes == std::vector<uint8_t>{0x45}) ++releases;
        if (sent.bytes == std::vector<uint8_t>{0x71}) ++queries;
    }
    assert(presses == 100 && releases == 100 && queries == 1);
}
static void independent_key_tests() {
    const auto F = amity::BusSide::Front, B = amity::BusSide::Back;
    constexpr uint8_t right = 0x04, volume_up = 0x41, volume_down = 0x42;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    h.configure({{"Separate", "", "Source", "Audio", "", 0},
                 {"Shared", "", "Source", "Source", "", 0}});
    // The same logical address on different buses is still two recipients.
    device(h, B, 5, 0x1000, 4, "Source");
    device(h, F, 5, 0x1000, 5, "Audio");
    assert(h.select_activity(0)); h.tick(1000); t.sent.clear();
    assert(h.press_key(volume_up, true));
    assert(h.press_key(right, true));
    assert(t.sent.size() == 2);
    assert(t.count(F, 5, {0x44, volume_up}) == 1);
    assert(t.count(B, 5, {0x44, right}) == 1);
    assert(!t.contains(F, 5, {0x45}) && !t.contains(B, 5, {0x45}));
    assert(h.release_key(right));
    assert(t.count(B, 5, {0x45}) == 1 && !t.contains(F, 5, {0x45}));
    assert(h.press_key(volume_up, true));
    h.tick(600000);
    assert(t.count(F, 5, {0x44, volume_up}) == 2 && !t.contains(F, 5, {0x71}));
    assert(h.release_key(volume_up)); // Release is admitted immediately.
    assert(t.count(F, 5, {0x45}) == 1);
    h.tick(1099999); assert(!t.contains(F, 5, {0x71}));
    h.tick(1100000); assert(t.count(F, 5, {0x71}) == 1);
    assert(h.release_key(right) && h.release_key(volume_up));
    assert(t.count(B, 5, {0x45}) == 1 && t.count(F, 5, {0x45}) == 1);

    // CEC releases carry no key operand. Ending either key at a shared
    // recipient must leave the other held, until the final key ends.
    assert(h.select_activity(1)); h.tick(2000000); t.sent.clear();
    assert(h.press_key(volume_up, true) && h.press_key(right, true));
    assert(h.release_key(right)); assert(!t.contains(B, 5, {0x45}));
    assert(h.press_key(volume_up, true)); assert(h.release_key(volume_up));
    assert(t.count(B, 5, {0x45}) == 1);
    t.sent.clear();
    assert(h.press_key(right, true) && h.press_key(volume_up, true));
    assert(h.release_key(volume_up)); assert(!t.contains(B, 5, {0x45}));
    h.tick(2499999); assert(!t.contains(B, 5, {0x71}));
    // Audio status can refresh after the audio key ends while Right remains
    // held, even when both keys share their recipient.
    h.tick(2500000); assert(t.count(B, 5, {0x71}) == 1);
    assert(h.press_key(right, true) && h.release_key(right));
    assert(t.count(B, 5, {0x45}) == 1);

    // One-shot commands cannot release another held key. Multiple audio
    // holds gate the coalesced query until the last audio key has ended.
    h.tick(3000000); t.sent.clear();
    assert(h.press_key(right, true)); assert(h.press_key(volume_up));
    assert(!t.contains(B, 5, {0x45}));
    assert(h.press_key(volume_up, true) && h.press_key(volume_down, true));
    assert(h.release_key(volume_up)); h.tick(4000000);
    assert(!t.contains(B, 5, {0x71}) && !t.contains(B, 5, {0x45}));
    assert(h.release_key(volume_down));
    h.tick(4499999); assert(!t.contains(B, 5, {0x71}));
    h.tick(4500000); assert(t.count(B, 5, {0x71}) == 1);
    assert(h.release_key()); assert(t.count(B, 5, {0x45}) == 1);

    // A rejected initial press creates no key state that could suppress a
    // later key's release or change a held toggle's original mapping.
    t.sent.clear(); t.reject_opcode = 0x44;
    assert(!h.press_key(volume_up, true)); t.reject_opcode = -1;
    assert(h.press_key(right, true) && h.release_key(right));
    assert(t.count(B, 5, {0x45}) == 1);
    t.sent.clear();
    assert(h.press_key(0x61, true)); assert(h.press_key(right, true));
    assert(h.press_key(0x61, true)); assert(h.release_key(right));
    assert(t.count(B, 5, {0x44, 0x44}) == 2 && !t.contains(B, 5, {0x44, 0x46}));
    assert(!t.contains(B, 5, {0x45})); assert(h.release_key(0x61));
    assert(t.count(B, 5, {0x45}) == 1);

    // Re-resolving a held key's device cannot move its repeat or final release.
    t.sent.clear(); assert(h.press_key(right, true));
    device(h, B, 8, 0x2000, 4, "Source"); t.sent.clear();
    assert(h.press_key(right, true) && h.release_key(right));
    assert(t.count(B, 5, {0x44, right}) == 1 && t.count(B, 5, {0x45}) == 1);
    assert(!t.contains(B, 8, {0x44, right}) && !t.contains(B, 8, {0x45}));
    t.sent.clear(); assert(h.press_key(right));
    assert(t.count(B, 8, {0x44, right}) == 1 && t.count(B, 8, {0x45}) == 1);

    // Activity changes still terminate every independent hold once per recipient.
    assert(h.select_activity(0)); t.sent.clear();
    assert(h.press_key(volume_up, true) && h.press_key(right, true));
    assert(h.select_activity(-1));
    assert(t.count(F, 5, {0x45}) == 1 && t.count(B, 8, {0x45}) == 1);
    t.sent.clear(); assert(h.release_key()); assert(t.sent.empty());
}
static void volume_sequence_tests() {
    amity::VolumeSequence volume;
    std::vector<std::pair<uint8_t, uint32_t>> accepted;
    bool admit = true;
    const auto send = [&](uint8_t key, uint32_t generation) {
        if (admit) accepted.emplace_back(key, generation);
        return admit;
    };
    volume.start({100, 7});
    volume.tick(250000, 7, send); volume.tick(500000, 7, send);
    assert(accepted.size() == 2 && !volume.idle());
    // Changing activity or its configuration cancels the 98 remaining steps.
    volume.tick(750000, 8, send);
    assert(accepted.size() == 2 && volume.idle());
    // Older adjustments already waiting in the application queue are stale,
    // even when a later activity happens to reuse the same input index.
    volume.start({50, 7}); volume.tick(1000000, 9, send);
    assert(accepted.size() == 2 && volume.idle());
    volume.start({-2, 9});
    admit = false; volume.tick(1250000, 9, send);
    assert(!volume.idle() && accepted.size() == 2);
    admit = true; volume.tick(1499999, 9, send); assert(accepted.size() == 2);
    volume.tick(1500000, 9, send); assert(!volume.idle());
    volume.tick(1750000, 9, send); assert(volume.idle());
    assert(accepted.size() == 4 && accepted[2] == std::make_pair(uint8_t{0x42}, uint32_t{9}) && accepted[3] == accepted[2]);

    Transport transport; amity::Hub hub(transport);
    const std::vector<amity::Activity> activities{{"First", "", "", "", "", 0}, {"Second", "", "", "", "", 0}};
    hub.configure(activities);
    hub.select_activity(0); const auto first = hub.activity_generation();
    hub.select_activity(0); assert(hub.activity_generation() == first); // Repair preserves the activity.
    hub.select_activity(1); assert(hub.activity_generation() != first);
    hub.select_activity(0); const auto restarted = hub.activity_generation();
    assert(restarted != first); // Returning to index zero cannot revive old work.
    hub.standby(); const auto standby = hub.activity_generation();
    assert(standby != restarted);
    hub.select_activity(0); assert(hub.activity_generation() != standby);
    hub.configure(activities); assert(hub.activity_generation() != restarted);
    assert(hub.snapshot().activity_generation == hub.activity_generation());
}
int main() {
    wire_tests(); hub_tests(); discovery_tests(); shared_power_tests(); playback_tests(); backpressure_tests(); independent_key_tests(); volume_sequence_tests();
    std::puts("CEC wire simulation and hub behavior tests passed");
}
