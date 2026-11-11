/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "cec_hub_fixture.hpp"
#include <algorithm>

using amity::BusSide;

static cec::Frame physical_report(uint8_t source, uint16_t physical, uint8_t type = 4) {
    cec::Frame frame;
    frame.size = 5; frame.data[0] = (source << 4) | 15; frame.data[1] = 0x84;
    frame.data[2] = physical >> 8; frame.data[3] = physical; frame.data[4] = type;
    return frame;
}

static cec::Frame physical_query(uint8_t destination) {
    cec::Frame frame;
    frame.size = 2; frame.data[0] = destination; frame.data[1] = 0x83;
    return frame;
}

static bool carries_physical(const cec::Frame &frame, uint8_t opcode, uint16_t physical) {
    return frame.size >= 4 && frame.data[1] == opcode && frame.data[2] == (physical >> 8) &&
           frame.data[3] == (physical & 255);
}

struct Capture : amity::Transport {
    struct Message { BusSide side; cec::Frame frame; };
    std::vector<Message> sent;
    bool send(BusSide side, uint8_t destination, const uint8_t *data, size_t size) override {
        cec::Frame frame; frame.size = size + 1; frame.data[0] = destination;
        std::copy(data, data + size, frame.data + 1);
        sent.push_back({side, frame}); return true;
    }
    void request_scan() override {}
};

static void core_selection() {
    // The final minimum is independent of the reply order, including an AVR
    // reporting after sources attached to several of its inputs.
    std::vector<uint16_t> addresses{0x2000, 0x2100, 0x2310};
    do {
        Capture wire; amity::Hub hub(wire);
        assert(fixture_observe(hub).front_physical_address == 0xffff && !fixture_observe(hub).ready);
        hub.set_addresses(4, 0, 0xffff);
        hub.announce(BusSide::Front);
        hub.receive(BusSide::Front, physical_query(4));
        amity::system::OwnedIntent announce;announce.kind=amity::system::OwnedIntentKind::announce_active_source;
        assert(hub.execute_owned(announce));
        assert(wire.sent.empty()); // No physical announcement, reply, or Active Source yet.

        hub.begin_discovery();
        uint8_t logical = 5;
        for (const auto physical : addresses) hub.receive(BusSide::Back, physical_report(logical++, physical));
        // Front topology cannot influence the isolated back-side inference.
        hub.receive(BusSide::Front, physical_report(0, 0x1000, 0));
        hub.determine_front_physical_address(0x5000);
        assert(fixture_observe(hub).front_physical_address == 0x2000 && fixture_observe(hub).ready);
        hub.finish_discovery();

        wire.sent.clear(); hub.announce(BusSide::Front);
        assert(wire.sent.size() == 3 && carries_physical(wire.sent[1].frame, 0x84, 0x2000));
        wire.sent.clear(); hub.receive(BusSide::Front, physical_query(4));
        assert(wire.sent.size() == 1 && carries_physical(wire.sent[0].frame, 0x84, 0x2000));
        wire.sent.clear();assert(hub.execute_owned(announce));
        assert(wire.sent.size()==1&&carries_physical(wire.sent[0].frame,0x82,0x2000));

        // Late broadcasts and ordinary rescans preserve selection, including
        // a changed fallback passed to the scan. Explicit settings apply below.
        hub.receive(BusSide::Back, physical_report(8, 0x1000));
        hub.begin_discovery(); hub.receive(BusSide::Back, physical_report(8, 0x1000));
        hub.determine_front_physical_address(0x3000); hub.finish_discovery();
        assert(fixture_observe(hub).front_physical_address == 0x2000);
    } while (std::next_permutation(addresses.begin(), addresses.end()));

    for (const std::vector<uint16_t> &reports : {std::vector<uint16_t>{},
                                              {0, 0xffff, 0x0100, 0x1010, 0x1201},
                                              {0xffff, 0, 0x2001}}) {
        Capture wire; amity::Hub hub(wire); hub.set_addresses(4, 0, 0xffff);
        hub.begin_discovery();
        hub.discovered(BusSide::Back, 5); // Poll ACK without a Report Physical Address.
        uint8_t logical = 6;
        for (auto physical : reports) hub.receive(BusSide::Back, physical_report(logical++, physical));
        hub.determine_front_physical_address(0x4000); hub.finish_discovery();
        assert(fixture_observe(hub).front_physical_address == 0x4000);
        hub.begin_discovery(); hub.receive(BusSide::Back, physical_report(5, 0x1000));
        hub.determine_front_physical_address(0x5000); hub.finish_discovery();
        assert(fixture_observe(hub).front_physical_address == 0x4000);
    }
    // Invalid low values must also be rejected when valid candidates exist.
    Capture wire; amity::Hub hub(wire); hub.set_addresses(4, 0, 0xffff); hub.begin_discovery();
    hub.receive(BusSide::Back, physical_report(4, 0x0100));
    hub.receive(BusSide::Back, physical_report(5, 0));
    hub.receive(BusSide::Back, physical_report(8, 0xffff));
    hub.receive(BusSide::Back, physical_report(9, 0x3100));
    hub.determine_front_physical_address(0x5000);
    assert(fixture_observe(hub).front_physical_address == 0x3100);

    // Numeric order is deliberately used: 1.2.0.0 is lower than 2.0.0.0,
    // even though the latter has more trailing zero digits. Both devices
    // reuse an OSD name, so deduplication removes the lower-address record;
    // its physical-address evidence must still contribute to the decision.
    Capture duplicate_wire; amity::Hub duplicate(duplicate_wire);
    duplicate.set_addresses(4, 0, 0xffff); duplicate.begin_discovery();
    cec::Frame name; name.size = 6; name.data[1] = 0x47;
    std::copy_n("Same", 4, name.data + 2);
    duplicate.receive(BusSide::Back, physical_report(5, 0x1200));
    name.data[0] = 0x50; duplicate.receive(BusSide::Back, name);
    duplicate.receive(BusSide::Back, physical_report(8, 0x2000));
    name.data[0] = 0x80; duplicate.receive(BusSide::Back, name);
    duplicate.determine_front_physical_address(0x5000); duplicate.finish_discovery();
    const auto deduplicated = fixture_observe(duplicate);
    assert(deduplicated.front_physical_address == 0x1200);
    assert(deduplicated.devices.size() == 1 && deduplicated.devices[0].physical_address == 0x2000);
}

static void live_default_selection() {
    Capture wire; amity::Hub fallback(wire);
    fallback.set_addresses(4, 0, 0xffff); fallback.begin_discovery();
    assert(!fallback.apply_default_front_physical_address(0x1000));
    assert(fixture_observe(fallback).front_physical_address == 0xffff); // Initial scan still owns selection.
    fallback.determine_front_physical_address(0x1000); fallback.finish_discovery();
    assert(!fallback.detected_front_physical_address());
    assert(fallback.apply_default_front_physical_address(0x2000));
    assert(fixture_observe(fallback).front_physical_address == 0x2000);
    assert(!fallback.apply_default_front_physical_address(0x2000));

    // Scanning updates the optional observation without changing selection.
    // An explicit default save then selects that scanned address first.
    fallback.begin_discovery(); fallback.receive(BusSide::Back, physical_report(5, 0x3000));
    fallback.determine_front_physical_address(0x2000); fallback.finish_discovery();
    assert(fallback.detected_front_physical_address() == 0x3000);
    assert(fixture_observe(fallback).front_physical_address == 0x2000);
    assert(fallback.apply_default_front_physical_address(0x4000));
    assert(fixture_observe(fallback).front_physical_address == 0x3000);
    assert(!fallback.apply_default_front_physical_address(0x5000));
    fallback.begin_discovery(); fallback.determine_front_physical_address(0x5000); fallback.finish_discovery();
    assert(!fallback.detected_front_physical_address() && fixture_observe(fallback).front_physical_address == 0x3000);
    assert(fallback.apply_default_front_physical_address(0x6000));
    assert(fixture_observe(fallback).front_physical_address == 0x6000);

    Capture detected_wire; amity::Hub detected(detected_wire);
    detected.set_addresses(4, 0, 0xffff); detected.begin_discovery();
    detected.receive(BusSide::Back, physical_report(5, 0x1000));
    detected.determine_front_physical_address(0x1000); detected.finish_discovery();
    // A scanned address equal to the old default still takes precedence.
    assert(detected.detected_front_physical_address() == 0x1000);
    assert(!detected.apply_default_front_physical_address(0x2000));
    assert(fixture_observe(detected).front_physical_address == 0x1000);
    detected.begin_discovery(); detected.determine_front_physical_address(0x2000); detected.finish_discovery();
    assert(!detected.detected_front_physical_address());
    assert(detected.apply_default_front_physical_address(0x3000));
    assert(fixture_observe(detected).front_physical_address == 0x3000);
}

static void reset_wire() {
    fixture_present = false; fixture_tick = 100000; fixture_time = 1000000;
    fixture_sent.clear(); fixture_transmitted.clear(); fixture_incoming.clear();
    fixture_send = {}; fixture_transmit = {}; fixture_delay = {};
}

static void service_boot(uint16_t reported, bool ack_only = false, bool delayed_avr = false) {
    reset_wire();
    bool back_report_requested = false, delayed_queued = false, saw_front_announcement = false;
    uint16_t announced_address = ack_only || !reported ? 0x5000 : reported;
    fixture_transmit = [&](int gpio, const cec::Frame &frame) {
        // This assertion also covers allocation polls on the front: even
        // claiming its logical address waits until after the back scan.
        if (gpio == 1 && !ack_only && reported) assert(back_report_requested);
        if (frame.source() == frame.destination()) return cec::TxResult::Nack;
        const bool present = gpio == 2 && (reported || ack_only) &&
                             (frame.destination() == 4 || (delayed_avr && frame.destination() == 5));
        return present ? cec::TxResult::Success : cec::TxResult::Nack;
    };
    fixture_send = [&](int gpio, const cec::Frame &frame) {
        if (frame.size == 1) return true;
        if (gpio == 2 && frame.destination() == 4 && frame.data[1] == 0x83) {
            back_report_requested = true;
            if (!ack_only && reported) fixture_incoming.push_back({2, physical_report(4, delayed_avr ? 0x2300 : reported)});
        }
        if (gpio == 2 && frame.destination() == 5 && frame.data[1] == 0x83 && delayed_avr)
            delayed_queued = true;
        if (gpio == 1 && frame.data[1] == 0x84) {
            saw_front_announcement = true;
            assert(carries_physical(frame, 0x84, announced_address));
        }
        return true;
    };
    fixture_delay = [&] {
        if (!delayed_queued) return;
        const bool back_polling_finished = std::any_of(fixture_transmitted.begin(), fixture_transmitted.end(),
            [](const auto &sent) {
                return sent.gpio == 2 && sent.frame.source() != sent.frame.destination() && sent.frame.destination() == 14;
            });
        if (!back_polling_finished) return;
        assert(!saw_front_announcement);
        for (const auto &poll : fixture_transmitted) assert(poll.gpio != 1);
        // The AVR reply arrives after all back polling, during the response
        // drain. Choosing before this drain would incorrectly pick 2.3.0.0.
        fixture_incoming.push_back({2, physical_report(5, reported, 5)});
        delayed_queued = false;
    };
    LocalHubService service;
    const auto boot = amity::system::BootId::parse("11111111111111111111111111111111").value();
    service.set_boot_identity(boot);
    assert(fixture_start(service,1,2,0x5000) == ESP_OK);
    assert(!service.adapter_runtime_snapshot()[0].effective_front_physical);
    // A pending allocation probe must leave the owner free to apply incoming
    // commands. Discovery continues only after its own completion arrives.
    fixture_async_autocomplete = false;
    assert(fixture_configure(service,{{"Watch", "TV", "", "", "", 0}})); run_one_command();
    assert(service.snapshot().activities.size() == 1 && !fixture_pending.empty());
    assert(fixture_adapter(service).front_physical_address == 0xffff);
    fixture_async_autocomplete = true;
    fixture_complete_all();
    fixture_finish_discovery(service);
    const auto expected = ack_only || !reported ? 0x5000 : reported;
    const auto state = fixture_adapter(service);
    assert(state.ready && !state.discovering && state.front_physical_address == expected);
    assert(state.discovery_generation >= 1 && saw_front_announcement);

    const bool from_default = ack_only || !reported;
    auto address_state = service.adapter_runtime_snapshot()[0];
    assert(from_default ? !address_state.observed_front_physical : address_state.observed_front_physical == reported);
    // Execute the real owner save path without altering the boot, cache,
    // logical addresses or discovery generation. A scanned address wins.
    announced_address = from_default ? 0x6000 : reported;
    fixture_sent.clear(); fixture_delay = {};
    const auto stopped = fixture_stopped_buses.size();
    unsigned writes = 0;
    assert(service.commit_adapter_settings(boot, {{0,0x6000}}, uint64_t(fixture_time)/1000 + 5000,
        [&]{ ++writes; return ESP_OK; }) == ESP_OK);
    assert(writes == 1 && !service.maintenance_ready() && fixture_stopped_buses.size() == stopped);
    const auto applied = fixture_adapter(service);
    assert(applied.front_physical_address == announced_address && applied.front_address == state.front_address && applied.back_address == state.back_address);
    assert(applied.discovery_generation == state.discovery_generation && applied.devices.size() == state.devices.size());
    for(size_t n=0;n<state.devices.size();++n){const auto& old=state.devices[n];const auto& current=applied.devices[n];
        assert(old.osd_name==current.osd_name && old.side==current.side && old.logical_address==current.logical_address && old.physical_address==current.physical_address);
    }
    address_state = service.adapter_runtime_snapshot()[0];
    assert(address_state.effective_front_physical == announced_address);
    if(from_default){assert(fixture_sent.size()==3 && carries_physical(fixture_sent[1].frame,0x84,announced_address));}
    else assert(fixture_sent.empty());

    fixture_sent.clear(); fixture_delay = {};
    fixture_incoming.push_back({1, physical_query(state.front_address)});
    fixture_incoming.push_back({2, physical_report(8, 0x1000)});
    assert(fixture_select(service,0)); run_one_command();
    for(unsigned pass=0;pass<256&&service.activity_setup_in_progress();++pass)run_idle(1);
    assert(!service.activity_setup_in_progress());
    bool identity = false, active = false;
    for (const auto &sent : fixture_sent) if (sent.gpio == 1) {
        identity |= carries_physical(sent.frame, 0x84, announced_address);
        active |= carries_physical(sent.frame, 0x82, announced_address);
    }
    assert(identity && active && fixture_adapter(service).front_physical_address == announced_address);
    const auto generation = service.snapshot().discovery_generation;
    reported = 0x1000; ack_only = false; delayed_avr = false;
    assert(service.discover()); run_one_command();
    fixture_finish_discovery(service, generation);
    assert(fixture_adapter(service).front_physical_address == announced_address);
    // The scan alone preserves selection. The next explicit save picks its
    // scanned address, regardless of the old selected address or default.
    const auto after_scan = service.adapter_runtime_snapshot()[0];
    assert(after_scan.observed_front_physical == 0x1000);
    announced_address = 0x1000;
    assert(service.standby()); run_one_command();
    for(unsigned pass=0;pass<64 && service.activity_setup_in_progress();++pass)run_idle(1);
    assert(service.commit_adapter_settings(boot, {{0,0x7000}}, uint64_t(fixture_time)/1000 + 5000,
        [&]{ ++writes; return ESP_OK; }) == ESP_OK);
    assert(writes == 2 && fixture_adapter(service).front_physical_address == 0x1000);
    assert(service.adapter_runtime_snapshot()[0].observed_front_physical == 0x1000);

    // A later completed scan with no usable address clears the observation.
    // Selection remains until a save, which now uses the requested default.
    const auto observed_generation=service.snapshot().discovery_generation;
    reported=0; ack_only=false; delayed_avr=false;
    assert(service.discover());run_one_command();fixture_finish_discovery(service,observed_generation);
    assert(!service.adapter_runtime_snapshot()[0].observed_front_physical);
    assert(fixture_adapter(service).front_physical_address==0x1000);
    announced_address=0x8000;
    assert(service.commit_adapter_settings(boot,{{0,0x8000}},uint64_t(fixture_time)/1000+5000,
        [&]{++writes;return ESP_OK;})==ESP_OK);
    assert(writes==3&&fixture_adapter(service).front_physical_address==0x8000);
    fixture_send = {}; fixture_transmit = {}; fixture_delay = {};
}

static void service_without_back_logical_address() {
    reset_wire();
    bool requested = false, announced = false;
    fixture_transmit = [&](int gpio, const cec::Frame &frame) {
        if (gpio == 1) {
            assert(requested);
            return cec::TxResult::Nack;
        }
        if (frame.source() == frame.destination()) {
            assert(frame.destination() == 0 || frame.destination() == 14);
            return cec::TxResult::Success; // All back logical-address candidates are occupied.
        }
        assert(frame.source() == 15 && !announced);
        return frame.destination() == 0 || frame.destination() == 5 || frame.destination() == 14 ?
               cec::TxResult::Success : cec::TxResult::Nack;
    };
    fixture_send = [&](int gpio, const cec::Frame &frame) {
        if (frame.size == 1) return true;
        if (gpio == 2) {
            // Physical-address replies are broadcast, so this query remains
            // valid from logical address 15. Directed-reply metadata queries
            // must be suppressed while Amity has no back logical address.
            assert(frame.source() == 15 && frame.destination() < 15 && frame.size == 2 && frame.data[1] == 0x83);
            if (frame.destination() == 5) {
                requested = true;
                fixture_incoming.push_back({2, physical_report(5, 0x6000, 5)});
            }
        } else if (frame.data[1] == 0x84) {
            assert(requested && carries_physical(frame, 0x84, 0x6000)); announced = true;
        }
        return true;
    };
    LocalHubService service; assert(fixture_start(service,1,2,0x1000) == ESP_OK);
    assert(fixture_configure(service,{})); run_one_command();
    fixture_finish_discovery(service);
    const auto state = fixture_adapter(service);
    assert(state.front_physical_address == 0x6000 && state.front_address < 15 && state.back_address == 15);
    assert(!state.ready && !state.discovering && requested && announced);
    fixture_send = {}; fixture_transmit = {};
}

static void service_claim_requires_two_nacks() {
    reset_wire();
    unsigned back_zero = 0, back_fourteen = 0, front_four = 0, front_eight = 0;
    fixture_transmit = [&](int gpio, const cec::Frame &frame) {
        if (frame.source() != frame.destination()) return cec::TxResult::Nack;
        if (gpio == 2 && frame.destination() == 0) {
            ++back_zero; return cec::TxResult::LineError;
        }
        if (gpio == 2 && frame.destination() == 14) {
            ++back_fourteen; return cec::TxResult::Nack;
        }
        if (gpio == 1 && frame.destination() == 4) {
            return ++front_four == 1 ? cec::TxResult::Nack : cec::TxResult::ArbitrationLost;
        }
        assert(gpio == 1 && frame.destination() == 8);
        ++front_eight; return cec::TxResult::Nack;
    };
    LocalHubService service; assert(fixture_start(service,1,2) == ESP_OK);
    assert(fixture_configure(service,{})); run_one_command(); fixture_finish_discovery(service);
    const auto state = fixture_adapter(service);
    assert(state.ready && state.back_address == 14 && state.front_address == 8);
    assert(back_zero == 1 && back_fourteen == 2 && front_four == 2 && front_eight == 2);
    fixture_transmit = {};
}

int main() {
    core_selection();
    live_default_selection();
    // Independent service instances model fresh power-ups, and must never
    // reuse the previous instance's discovered or fallback physical address.
    service_boot(0x2000, false, true);
    service_boot(0x3000);
    service_boot(0x5000); // Detected address numerically equals the saved fallback.
    service_boot(0); // No physical-address replies.
    service_boot(0x2000, true); // Present device ACKs but supplies no metadata.
    service_without_back_logical_address();
    service_claim_requires_two_nacks();
    std::puts("Front physical-address selection, boot ordering, fallback, identity, and pinning tests passed");
}
