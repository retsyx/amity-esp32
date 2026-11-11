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
        const auto diagnostic = w.a.diagnostic();
        assert(diagnostic.reason == cec::LineErrorReason::TickGap);
        assert(diagnostic.at_us == 20000 && diagnostic.tick_gap_us == 1050);
        assert(diagnostic.elapsed_us == 3200 && diagnostic.slot == -1 && diagnostic.within_us == 3200);
        assert(!diagnostic.line_high && w.a.started() && !w.a.busy());
        assert(w.a.take_result() == cec::TxResult::Pending);
        assert(w.a.begin(frame({0x4f, 0x36}), w.now));
        assert(w.a.diagnostic().reason == cec::LineErrorReason::None && !w.a.started());
        assert(diagnostic.reason == cec::LineErrorReason::TickGap && diagnostic.at_us == 20000);
    }
    { // A delayed data callback records the actual missed position, not the
      // previous tick's slot, while keeping the original abort threshold.
        Wire w; assert(w.a.begin(frame({0x4f, 0x36}), 0)); w.run(30000);
        w.now += 1000; w.tick();
        assert(w.a.take_result() == cec::TxResult::LineError);
        const auto diagnostic = w.a.diagnostic();
        assert(diagnostic.reason == cec::LineErrorReason::TickGap && diagnostic.tick_gap_us == 1050);
        assert(diagnostic.at_us == 31000 && diagnostic.elapsed_us == 14200);
        assert(diagnostic.slot == 4 && diagnostic.within_us == 100);
    }
    { // Holding a zero-valued data bit low past 1.8 ms is a different
      // LineError even with uninterrupted 50 us callbacks.
        cec::Engine e; assert(e.begin(frame({0x4f, 0x36}), 0));
        bool low = false;
        for (uint64_t now = 0; now <= 23100; now += 50) {
            const bool external_low = now >= 22700;
            low = e.tick(now, !(low || external_low));
        }
        assert(e.take_result() == cec::TxResult::LineError && e.started());
        const auto diagnostic = e.diagnostic();
        assert(diagnostic.reason == cec::LineErrorReason::LowLine && diagnostic.at_us == 23100);
        assert(diagnostic.tick_gap_us == 50 && diagnostic.elapsed_us == 6300);
        assert(diagnostic.slot == 0 && diagnostic.within_us == 1800 && !diagnostic.line_high);
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
namespace m=amity::system;
static m::QualifiedDeviceName named(const std::string& name,amity::BusSide side=amity::BusSide::Back){
    return {{{},{},0,side==amity::BusSide::Front?m::Side::front:m::Side::back},name};
}
static void context(amity::Hub& hub,const std::string& source,const std::string& audio={},const std::string& display={}){
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::context;intent.selected=!source.empty()||!display.empty();
    if(!source.empty())intent.source=named(source);
    if(!audio.empty())intent.audio=named(audio);
    if(!display.empty())intent.display=named(display,amity::BusSide::Front);
    assert(hub.execute_owned(intent));
}
static bool press(amity::Hub& hub,uint8_t key,const std::string& name="Source"){
    const auto* found=hub.find_device(name);if(!found)return false;
    const auto recipient=*found;const bool audio=amity::Hub::audio_key(key);
    if(!hub.transmit_key(recipient,hub.translated_key(key),audio))return false;
    hub.transmit_release(recipient,audio);return true;
}
static void hub_tests() {
    using amity::BusSide;
    const auto F=BusSide::Front,B=BusSide::Back;
    Transport t;amity::Hub h(t);h.set_addresses(4,0);
    h.announce(F);
    assert(t.sent.size()==3);
    assert((t.sent[0].bytes==std::vector<uint8_t>{0xa6,6,0x10,0,0}));
    assert((t.sent[1].bytes==std::vector<uint8_t>{0x84,0x10,0,4}));
    assert((t.sent[2].bytes==std::vector<uint8_t>{0x87,0,0,0}));
    device(h,F,0,0,0,"Screen");device(h,B,4,0x1500,4,"Apple TV");
    device(h,B,11,0x1400,4,"PS5");device(h,B,5,0x1000,5,"AVR");
    context(h,"PS5","AVR","Screen");t.sent.clear();
    m::OwnedIntent intent;intent.kind=m::OwnedIntentKind::power;intent.target=named("Screen",F);intent.on=true;
    assert(h.execute_owned(intent)&&t.contains(F,0,{0x04}));
    intent.target=named("Apple TV");assert(h.execute_owned(intent));
    assert(t.contains(B,4,{0x44,0x6d})&&t.contains(B,4,{0x45}));
    intent.on=false;assert(h.execute_owned(intent)&&t.contains(B,4,{0x36}));
    intent.kind=m::OwnedIntentKind::source_input;intent.target=named("PS5");
    assert(h.execute_owned(intent)&&t.contains(B,15,{0x86,0x14,0}));
    intent.kind=m::OwnedIntentKind::switch_input;intent.target=named("AVR");intent.input=3;
    assert(h.execute_owned(intent)&&t.contains(B,5,{0x44,0x69,3}));
    assert(press(h,0x41,"AVR"));h.tick(500000);assert(t.contains(B,5,{0x71}));
    h.receive(B,frame({0x50,0x7a,0x80|42}));
    assert(h.snapshot().audio_status_known&&h.snapshot().muted&&h.snapshot().volume==42);
    assert(press(h,0x66,"AVR")&&!h.snapshot().audio_status_known);
    h.receive(B,frame({0x50,0x7a,127}));
    assert(h.snapshot().audio_status_known&&!h.snapshot().muted&&h.snapshot().volume==127);
    t.sent.clear();h.receive(F,frame({0x04,0x8f}));assert(t.contains(F,0,{0x90,0}));
    h.receive(B,frame({0x40,0x8f}));assert(t.contains(B,4,{0x90,1}));
    h.receive(B,frame({0xb0,0x8f}));assert(t.contains(B,11,{0x90,0}));
    t.sent.clear();h.receive(B,frame({0x4f,0x82,0x15,0}));
    assert(t.contains(B,4,{0x36})&&t.contains(B,15,{0x86,0x14,0}));
    t.sent.clear();h.receive(B,frame({0xbf,0x82,0x14,0}));assert(t.sent.empty());
    h.receive(B,frame({0xbf,0x84,0x13,0,4}));assert(t.contains(B,15,{0x86,0x13,0}));
    context(h,"");t.sent.clear();h.receive(B,frame({0x4f,0x82,0x15,0}));
    assert(t.contains(F,15,{0x36})&&t.contains(B,15,{0x36}));
    t.sent.clear();h.receive(F,frame({0x04,0x83}));assert(t.contains(F,15,{0x84,0x10,0,4}));
    h.receive(B,frame({0x40,0x83}));assert(t.contains(B,15,{0x84,0,0,0}));
    h.receive(F,frame({0x04,0x9f}));assert(t.contains(F,0,{0x9e,6}));
    h.receive(B,frame({0x40,0xa5}));assert(t.contains(B,15,{0xa6,6,0x80,0,0}));
    h.receive(F,frame({0x04,0x46}));assert(t.contains(F,0,{0x47,'a','m','i','t','y'}));
    t.sent.clear();
    for(const auto& f:{frame({0x0f,0x83}),frame({0xf4,0x8f}),frame({0x04,0xfe}),
                      frame({0x04,0,0xfe,0}),frame({0x4f,0x04}),frame({0x40,0x82,0x15,0})})h.receive(F,f);
    assert(t.sent.empty());
    h.receive(F,frame({0x04,0xff}));assert(t.contains(F,0,{0,0xff,4}));
    // Manufacturer identity does not change power translation.
    h.receive(B,frame({0x4f,0x87,0,0x12,0x34}));t.sent.clear();
    intent.kind=m::OwnedIntentKind::power;intent.target=named("Apple TV");intent.on=true;
    assert(h.execute_owned(intent));assert(t.contains(B,4,{0x44,0x6d})&&!t.contains(B,4,{0x44,0x40}));
    device(h,B,8,0,4,"Duplicate PA");
    assert(h.find_device("Screen",F));
}
static void discovery_tests() {
    const auto F = amity::BusSide::Front, B = amity::BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    assert(!h.find_device("Source",B));
    device(h, F, 0, 0, 0, "Screen"); device(h, B, 4, 0x1000, 4, "Source");
    assert(h.find_device("Source",B));
    context(h,"Source","Source","Screen"); assert(h.find_device("Source",B));
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
    t.sent.clear(); press(h,0); assert(t.contains(B, 8, {0x44, 0}));
    // A selected source reconnects at another logical and physical address.
    // Its late OSD reply must replace the old name and restore the new route.
    t.sent.clear();
    h.receive(B, frame({0xbf, 0x84, 0x30, 0, 4}));
    assert(!t.contains(B, 15, {0x86, 0x30, 0}));
    h.receive(B, frame({0xb0, 0x47, 'S', 'o', 'u', 'r', 'c', 'e'}));
    assert(t.contains(B, 15, {0x86, 0x30, 0}));
    assert(h.snapshot().devices.size() == 2);
    t.sent.clear(); press(h,0); assert(t.contains(B, 11, {0x44, 0}));
    // The opposite metadata reply order must never route to unknown PA ffff.
    h.discovered(B, 4);
    t.sent.clear(); h.receive(B, frame({0x40, 0x47, 'S', 'o', 'u', 'r', 'c', 'e'}));
    assert(!t.contains(B, 15, {0x86, 0xff, 0xff}));
    h.receive(B, frame({0x4f, 0x84, 0x40, 0, 4}));
    assert(t.contains(B, 15, {0x86, 0x40, 0}));
    assert(h.snapshot().devices.size() == 2);
    h.clear_devices(); assert(!h.find_device("Source",B));
}
static void playback_tests() {
    const auto B = amity::BusSide::Back;
    Transport t; amity::Hub h(t); h.set_addresses(4, 0);
    device(h, B, 4, 0x1000, 4, "Source"); context(h,"Source");
    device(h, B, 8, 0x2000, 4, "Source B");
    const auto check = [&](std::initializer_list<uint8_t> keys, std::initializer_list<uint8_t> expected) {
        t.sent.clear();
        for (const auto key : keys) assert(press(h,key));
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
    // Rejected explicit Pause/Stop cannot change a previously accepted Play.
    for (const auto key : {0x46, 0x45}) {
        assert(press(h,0x44)); t.reject_opcode = 0x44; assert(!press(h,key));
        t.reject_opcode = -1; check({0x61}, {0x46});
    }
    // Rejected explicit Play likewise cannot turn a paused state into playing.
    t.reject_opcode = 0x44; assert(!press(h,0x44));
    t.reject_opcode = -1; check({0x61}, {0x44});
    // Playback intent belongs to the selected source, so a new source starts
    // with Play. Activities sharing that source retain the accepted state.
    context(h,"Source B"); t.sent.clear(); assert(press(h,0x61,"Source B"));
    assert(t.contains(B, 8, {0x44, 0x44}) && !t.contains(B, 8, {0x44, 0x46}));
    context(h,"Source B"); t.sent.clear(); assert(press(h,0x61,"Source B"));
    assert(t.contains(B, 8, {0x44, 0x46}) && !t.contains(B, 8, {0x44, 0x44}));
}
static void backpressure_tests(){
    const auto B=amity::BusSide::Back;
    Transport t;amity::Hub h(t);h.set_addresses(4,0);device(h,B,4,0x1000,4,"Source");context(h,"Source","Source");
    const auto recipient=*h.find_device("Source",B);
    t.sent.clear();t.reject_opcode=0x45;
    assert(h.transmit_key(recipient,0,false)&&!h.transmit_release(recipient,false));
    assert(h.snapshot().uncertain_releases==1);
    t.reject_opcode=-1;h.tick(1000000);assert(!t.contains(B,4,{0x45}));
    m::OwnedIntent power;power.kind=m::OwnedIntentKind::power;power.target=named("Source");power.on=true;
    t.sent.clear();t.reject_opcode=0x45;assert(!h.execute_owned(power));
    assert(t.contains(B,4,{0x44,0x6d})&&!t.contains(B,4,{0x45}));
    t.reject_opcode=-1;h.tick(2000000);assert(!t.contains(B,4,{0x45}));
    t.sent.clear();t.reject_opcode=0x44;assert(!h.execute_owned(power));assert(!t.contains(B,4,{0x45}));
    t.reject_opcode=-1;t.sent.clear();
    for(uint64_t i=1;i<=100;++i){h.tick(i*250000);assert(press(h,0x41));}
    assert(!t.contains(B,4,{0x71}));h.tick(25499999);assert(!t.contains(B,4,{0x71}));
    h.tick(25500000);assert(t.count(B,4,{0x71})==1);
    assert(t.count(B,4,{0x44,0x41})==100&&t.count(B,4,{0x45})==100);
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
}
static void display_audio_fallback_tests(){
    const auto F=amity::BusSide::Front,B=amity::BusSide::Back;
    Transport wire;amity::Hub hub(wire);hub.set_addresses(4,0);
    device(hub,F,0,0,0,"Screen");device(hub,B,4,0x3200,4,"Player");device(hub,B,5,0x3000,5,"Receiver");
    context(hub,"Player","","Screen");hub.tick(1000);wire.sent.clear();
    for(uint8_t key:{0x41,0x42,0x43,0x65,0x66}){
        assert(press(hub,key,"Screen"));assert(wire.count(F,0,{0x44,key})==1);
        assert(!wire.contains(B,4,{0x44,key})&&!wire.contains(B,5,{0x44,key}));
    }
    assert(wire.count(F,0,{0x45})==5);hub.tick(501000);assert(wire.count(F,0,{0x71})==1);
    hub.receive(F,frame({0x04,0x7a,23}));assert(hub.snapshot().audio_status_known&&hub.snapshot().volume==23);
    hub.receive(B,frame({0x50,0x7a,99}));assert(hub.snapshot().volume==23);
    context(hub,"Player","Receiver","Screen");wire.sent.clear();
    assert(press(hub,0x42,"Receiver")&&press(hub,0,"Player"));
    assert(wire.count(B,5,{0x44,0x42})==1&&wire.count(B,4,{0x44,0})==1);
    context(hub,"Player","Absent","Screen");wire.sent.clear();assert(!press(hub,0x41,"Absent")&&wire.sent.empty());
    context(hub,"Player");wire.sent.clear();assert(!press(hub,0x41,"Absent")&&wire.sent.empty());assert(press(hub,0,"Player"));
}
int main() {
    wire_tests(); hub_tests(); discovery_tests(); playback_tests(); backpressure_tests(); volume_sequence_tests();display_audio_fallback_tests();
    std::puts("CEC wire simulation and hub behavior tests passed");
}
