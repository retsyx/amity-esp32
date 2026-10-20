/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "cec/board_profile.hpp"
#include "cec/scheduler.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static cec::Frame frame(std::initializer_list<uint8_t> bytes) {
    cec::Frame f;
    for (auto b : bytes) f.data[f.size++] = b;
    return f;
}
static bool same(const cec::Frame &a, const cec::Frame &b) {
    return a.size == b.size && !std::memcmp(a.data, b.data, a.size);
}
struct Domain {
    cec::Engine engines[3];
    cec::EngineBank bank;
    uint64_t now = 0, low = 0, ever_low = 0;
    Domain() { for (unsigned i = 0; i < 3; ++i) assert(bank.attach(engines[i], i) >= 0); }
    void run(uint64_t duration) {
        const auto end = now + duration;
        for (; now < end; now += 50) {
            low = bank.tick(now, low ? 0 : 7);
            ever_low |= low;
        }
    }
};
static void observation_tests() {
    { // A monitor observes another device's directed frame without any ACK.
        Domain d;
        d.engines[0].set_address(4); d.engines[1].set_address(0);
        assert(d.engines[2].install_receive_policy({true, 15}));
        const auto f = frame({0x40, 0x8f});
        assert(d.engines[0].begin(f, d.now)); d.run(100000);
        assert(d.engines[0].take_result() == cec::TxResult::Success);
        assert(!(d.ever_low & 4));
        cec::ReceiveObservation o;
        assert(d.engines[2].take_observation(o) && same(o.frame, f));
        assert(!o.addressed_to_local_identity && o.local_rx_sequence == 1 && o.local_monotonic_us > 0);
        assert(!d.engines[0].take_observation(o));
    }
    { // Delivery does not turn a vacant address into an occupied address.
        Domain d; d.engines[0].set_address(4);
        assert(d.engines[2].install_receive_policy({true, 15}));
        const auto f = frame({0x48});
        assert(d.engines[0].begin(f, 0)); d.run(100000);
        assert(d.engines[0].take_result() == cec::TxResult::Nack);
        assert(!(d.ever_low & 4));
        cec::ReceiveObservation o;
        assert(d.engines[2].take_observation(o) && same(o.frame, f));
        assert(!o.addressed_to_local_identity);
        assert(!d.engines[1].take_observation(o));
        assert(!d.engines[2].install_receive_policy({true, 16}));
    }
    { // Replacement after the header ACK cannot change a frame's identity.
        Domain d; d.engines[0].set_address(4); d.engines[1].set_address(0);
        const auto f = frame({0x40, 0x44, 0x6d});
        assert(d.engines[0].begin(f, 0)); d.run(46500);
        assert(d.engines[1].install_receive_policy({true, 5}));
        d.run(120000);
        assert(d.engines[0].take_result() == cec::TxResult::Success);
        cec::ReceiveObservation o;
        assert(d.engines[1].take_observation(o) && same(o.frame, f) && o.addressed_to_local_identity);
        assert(d.engines[0].begin(frame({0x40}), d.now)); d.run(100000);
        assert(d.engines[0].take_result() == cec::TxResult::Nack);
        assert(d.engines[1].take_observation(o) && !o.addressed_to_local_identity && o.local_rx_sequence == 2);
        assert(d.engines[0].begin(frame({0x45}), d.now)); d.run(100000);
        assert(d.engines[0].take_result() == cec::TxResult::Success);
        assert(d.engines[1].take_observation(o) && o.addressed_to_local_identity && o.local_rx_sequence == 3);
    }
    { // Same-domain arbitration still works when every local engine shares a timer.
        Domain d; d.engines[0].set_address(4); d.engines[1].set_address(8); d.engines[2].set_address(0);
        const auto f = frame({0x40, 0x8f});
        assert(d.engines[0].begin(f, 0)); assert(d.engines[1].begin(frame({0x80, 0x46}), 0));
        d.run(150000);
        assert(d.engines[0].take_result() == cec::TxResult::Success);
        assert(d.engines[1].take_result() == cec::TxResult::ArbitrationLost);
        cec::Frame rx; assert(d.engines[2].take_received(rx) && same(rx, f));
    }
}
static void parallel_line_tests() {
    cec::Engine local[10], peer[10], overflow;
    cec::EngineBank bank;
    for (unsigned i = 0; i < 10; ++i) {
        assert(bank.attach(local[i], static_cast<uint8_t>(i + 32)) == static_cast<int>(i));
        local[i].set_address(4); peer[i].set_address(0);
    }
    assert(bank.attach(overflow, 1) == -1);
    assert(bank.attach(local[0], 1) == -1);
    auto f = frame({0x40, 0x47, 0x55, 0xaa, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    for (auto &e : local) assert(e.begin(f, 0));
    uint64_t low = 0; bool peer_low[10]{};
    for (uint64_t now = 0; now < 500000; now += 50) {
        uint64_t levels = 0; bool high[10];
        for (unsigned i = 0; i < 10; ++i) {
            high[i] = !(peer_low[i] || (low & (uint64_t{1} << (i + 32))));
            if (high[i]) levels |= uint64_t{1} << (i + 32);
        }
        low = bank.tick(now, levels);
        for (unsigned i = 0; i < 10; ++i) peer_low[i] = peer[i].tick(now, high[i]);
    }
    for (unsigned i = 0; i < 10; ++i) {
        assert(local[i].take_result() == cec::TxResult::Success);
        cec::Frame rx; assert(peer[i].take_received(rx) && same(rx, f));
    }
    // A stuck line expires locally while the nine other lines complete.
    for (auto &e : local) assert(e.begin(frame({0x4f, 0x36}), 500000));
    low = 0;
    for (uint64_t now = 500000; now < 1600000; now += 50) {
        const uint64_t levels = ~(low | (uint64_t{1} << 32));
        low = bank.tick(now, levels);
    }
    assert(local[0].take_result() == cec::TxResult::Timeout);
    for (unsigned i = 1; i < 10; ++i) assert(local[i].take_result() == cec::TxResult::Success);
    bank.detach(local[0]);
    assert(bank.attach(overflow, 32) == 0);
}
static bool valid_gpio(int gpio) { return gpio >= 0 && gpio <= 48 && gpio != 46; }
static void profile_tests() {
    using E = cec::ProfileError;
    auto p = cec::legacy_board2_profile;
    assert(cec::validate_board_profile(p, valid_gpio) == E::None);
    assert(cec::validate_board_profile(cec::legacy_buttonless_profile, valid_gpio) == E::None);
    p.ports[1].gpio = 1; assert(cec::validate_board_profile(p, valid_gpio) == E::DuplicateGpio);
    p = cec::legacy_board2_profile; p.ports[1].side = cec::Side::Front;
    assert(cec::validate_board_profile(p, valid_gpio) == E::InvalidConnector);
    p = cec::legacy_board2_profile; p.ports[1].gpio = 46;
    assert(cec::validate_board_profile(p, valid_gpio) == E::InvalidGpio);
    p = cec::legacy_board2_profile; p.ports[1].gpio = 4;
    assert(cec::validate_board_profile(p, valid_gpio) == E::PeripheralConflict);
    p = cec::legacy_board2_profile; const int reserved[] = {2};
    assert(cec::validate_board_profile(p, valid_gpio, reserved, 1) == E::PeripheralConflict);
    const int button_reserved[] = {4};
    assert(cec::validate_board_profile(p, valid_gpio, button_reserved, 1) == E::PeripheralConflict);
    p.revision = 2; assert(cec::validate_board_profile(p, valid_gpio) == E::Unsupported);
    p = cec::legacy_board2_profile; p.tentacle_count = 0;
    assert(cec::validate_board_profile(p, valid_gpio) == E::InvalidCount);
    p = cec::legacy_board2_profile; p.ports[1].gpio = 3;
    assert(cec::validate_board_profile(p, valid_gpio) == E::Unsupported);
    cec::BoardProfile development{cec::BoardProfileId::Development5Tentacle,1,5,{},-1,-1};
    for(unsigned id=0;id<5;++id){development.ports[2*id]={static_cast<uint8_t>(id),cec::Side::Front,-1};development.ports[2*id+1]={static_cast<uint8_t>(id),cec::Side::Back,-1};}
    assert(cec::validate_board_profile(development,valid_gpio)==E::None); // No adapters configured is valid, with no guessed GPIOs.
    assert(!cec::connector_enabled(development,0)&&!cec::connector_enabled(development,4));
    development.ports[8].gpio=7;assert(cec::validate_board_profile(development,valid_gpio)==E::InvalidConnector);
    development.ports[9].gpio=8;assert(cec::validate_board_profile(development,valid_gpio)==E::None);
    assert(!cec::connector_enabled(development,0)&&cec::connector_enabled(development,4)); // Sparse connector5 retains index4.
    development.presence[4]={9,false,true,false};
    assert(cec::validate_board_profile(development,valid_gpio,nullptr,0,valid_gpio)==E::None);
    development.presence[4].pulldown=true;assert(cec::validate_board_profile(development,valid_gpio,nullptr,0,valid_gpio)==E::InvalidGpio);
    development.presence[4]={7,true,false,false};assert(cec::validate_board_profile(development,valid_gpio,nullptr,0,valid_gpio)==E::DuplicateGpio);
    development.presence[4]={9,true,false,false};const int occupied[]={9};assert(cec::validate_board_profile(development,valid_gpio,occupied,1,valid_gpio)==E::PeripheralConflict);
    development.presence[0]={10,true,false,false};assert(cec::validate_board_profile(development,valid_gpio,nullptr,0,valid_gpio)==E::InvalidGpio);
}
int main() {
    observation_tests(); parallel_line_tests(); profile_tests();
    std::puts("CEC monitor ownership, frame-atomic policy, ten-engine simulation and board validation passed");
}
