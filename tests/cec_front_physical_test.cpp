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
        assert(hub.snapshot().front_physical_address == 0xffff && !hub.snapshot().ready);
        hub.set_addresses(4, 0, 0xffff);
        hub.announce(BusSide::Front);
        hub.receive(BusSide::Front, physical_query(4));
        hub.configure({{"Watch", "", "", "", "", 0}});
        assert(hub.select_activity(0));
        assert(wire.sent.empty()); // No physical announcement, reply, or Active Source yet.

        hub.begin_discovery();
        uint8_t logical = 5;
        for (const auto physical : addresses) hub.receive(BusSide::Back, physical_report(logical++, physical));
        // Front topology cannot influence the isolated back-side inference.
        hub.receive(BusSide::Front, physical_report(0, 0x1000, 0));
        hub.determine_front_physical_address(0x5000);
        assert(hub.snapshot().front_physical_address == 0x2000 && hub.snapshot().ready);
        hub.finish_discovery();

        wire.sent.clear(); hub.announce(BusSide::Front);
        assert(wire.sent.size() == 3 && carries_physical(wire.sent[1].frame, 0x84, 0x2000));
        wire.sent.clear(); hub.receive(BusSide::Front, physical_query(4));
        assert(wire.sent.size() == 1 && carries_physical(wire.sent[0].frame, 0x84, 0x2000));
        wire.sent.clear(); assert(hub.select_activity(0));
        assert(wire.sent.size() == 1 && carries_physical(wire.sent[0].frame, 0x82, 0x2000));

        // Late broadcasts, a completed rescan, and changed defaults cannot
        // replace either a discovered or a fallback decision during a boot.
        hub.receive(BusSide::Back, physical_report(8, 0x1000));
        hub.begin_discovery(); hub.receive(BusSide::Back, physical_report(8, 0x1000));
        hub.determine_front_physical_address(0x3000); hub.finish_discovery();
        assert(hub.snapshot().front_physical_address == 0x2000);
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
        assert(hub.snapshot().front_physical_address == 0x4000);
        hub.begin_discovery(); hub.receive(BusSide::Back, physical_report(5, 0x1000));
        hub.determine_front_physical_address(0x5000); hub.finish_discovery();
        assert(hub.snapshot().front_physical_address == 0x4000);
    }
    // Invalid low values must also be rejected when valid candidates exist.
    Capture wire; amity::Hub hub(wire); hub.set_addresses(4, 0, 0xffff); hub.begin_discovery();
    hub.receive(BusSide::Back, physical_report(4, 0x0100));
    hub.receive(BusSide::Back, physical_report(5, 0));
    hub.receive(BusSide::Back, physical_report(8, 0xffff));
    hub.receive(BusSide::Back, physical_report(9, 0x3100));
    hub.determine_front_physical_address(0x5000);
    assert(hub.snapshot().front_physical_address == 0x3100);

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
    const auto deduplicated = duplicate.snapshot();
    assert(deduplicated.front_physical_address == 0x1200);
    assert(deduplicated.devices.size() == 1 && deduplicated.devices[0].physical_address == 0x2000);
}

static void reset_wire() {
    fixture_present = false; fixture_tick = 100000; fixture_time = 1000000;
    fixture_sent.clear(); fixture_transmitted.clear(); fixture_incoming.clear();
    fixture_send = {}; fixture_transmit = {}; fixture_delay = {};
}

static void service_boot(uint16_t reported, bool ack_only = false, bool delayed_avr = false) {
    reset_wire();
    bool back_report_requested = false, delayed_queued = false, saw_front_announcement = false;
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
        if (gpio == 2 && frame.destination() == 4 && frame.data[1] == 0x83) {
            back_report_requested = true;
            if (!ack_only && reported) fixture_incoming.push_back({2, physical_report(4, delayed_avr ? 0x2300 : reported)});
        }
        if (gpio == 2 && frame.destination() == 5 && frame.data[1] == 0x83 && delayed_avr)
            delayed_queued = true;
        if (gpio == 1 && frame.data[1] == 0x84) {
            saw_front_announcement = true;
            assert(carries_physical(frame, 0x84, ack_only || !reported ? 0x5000 : reported));
        }
        return true;
    };
    fixture_delay = [&] {
        if (!delayed_queued) return;
        assert(!saw_front_announcement);
        for (const auto &poll : fixture_transmitted) assert(poll.gpio != 1);
        // The AVR reply arrives after all back polling, during the response
        // drain. Choosing before this drain would incorrectly pick 2.3.0.0.
        fixture_incoming.push_back({2, physical_report(5, reported, 5)});
        delayed_queued = false;
    };
    amity::HubService service;
    assert(service.start(1, 2, 0x5000) == ESP_OK);
    assert(service.snapshot().front_physical_address == 0xffff);
    assert(service.configure({{"Watch", "", "", "", "", 0}})); run_one_command();
    const auto expected = ack_only || !reported ? 0x5000 : reported;
    const auto state = service.snapshot();
    assert(state.ready && !state.discovering && state.front_physical_address == expected);
    assert(state.discovery_generation >= 1 && saw_front_announcement);

    fixture_sent.clear(); fixture_delay = {};
    fixture_incoming.push_back({1, physical_query(state.front_address)});
    fixture_incoming.push_back({2, physical_report(8, 0x1000)});
    assert(service.select_activity(0)); run_one_command();
    bool identity = false, active = false;
    for (const auto &sent : fixture_sent) if (sent.gpio == 1) {
        identity |= carries_physical(sent.frame, 0x84, expected);
        active |= carries_physical(sent.frame, 0x82, expected);
    }
    assert(identity && active && service.snapshot().front_physical_address == expected);
    assert(service.discover()); run_one_command();
    assert(service.snapshot().front_physical_address == expected);
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
    amity::HubService service; assert(service.start(1, 2, 0x1000) == ESP_OK);
    assert(service.configure({})); run_one_command();
    const auto state = service.snapshot();
    assert(state.front_physical_address == 0x6000 && state.front_address < 15 && state.back_address == 15);
    assert(!state.ready && !state.discovering && requested && announced);
    fixture_send = {}; fixture_transmit = {};
}

int main() {
    core_selection();
    // Independent service instances model fresh power-ups, and must never
    // reuse the previous instance's discovered or fallback physical address.
    service_boot(0x2000, false, true);
    service_boot(0x3000);
    service_boot(0); // No physical-address replies.
    service_boot(0x2000, true); // Present device ACKs but supplies no metadata.
    service_without_back_logical_address();
    std::puts("Front physical-address selection, boot ordering, fallback, identity, and pinning tests passed");
}
