/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "key_sequence.hpp"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

static void swipe_tests() {
    amity::KeySequence sequence(10);
    std::vector<std::pair<uint8_t,uint32_t>> sent;
    bool admit=true;
    auto send=[&](uint8_t key,uint32_t generation) {
        if(!admit)return false;
        sent.emplace_back(key,generation);
        return true;
    };
    sequence.add(0x04,3,7);
    sequence.tick(249999,7,send);assert(sent.empty());
    sequence.tick(250000,7,send);assert(sent.size()==1&&!sequence.idle());
    sequence.tick(250001,7,send);assert(sent.size()==1);
    admit=false;sequence.tick(500000,7,send);assert(sent.size()==1);
    admit=true;sequence.tick(750000,7,send);sequence.tick(1000000,7,send);
    assert(sent.size()==3&&sequence.idle());
    for(const auto &step:sent)assert(step.first==0x04&&step.second==7);

    // Several detections coalesce per direction, with Python's ten-step limit.
    sequence.add(0x01,6,7);sequence.add(0x01,6,7);sequence.add(0x02,2,7);
    sequence.add(0x03,1,7);sequence.add(0x04,1,7);sequence.add(0x01,6,7);
    for(unsigned i=0;i<20;++i)sequence.tick(1250000+i*250000,7,send);
    assert(sent.size()==17&&sequence.idle());
    for(size_t i=3;i<13;++i)assert(sent[i].first==0x01);
    assert(sent[13].first==0x02&&sent[14].first==0x02);
    assert(sent[15].first==0x03&&sent[16].first==0x04);

    // A transition drops the remaining inertia instead of navigating the new activity.
    sequence.add(0x04,6,7);sequence.tick(7000000,8,send);
    assert(sequence.idle()&&sent.size()==17);
    sequence.add(0x02,6,7);sequence.add(0x03,1,8);sequence.tick(7250000,8,send);
    assert(sequence.idle()&&sent.size()==18);
    assert(sent.back().first==0x03&&sent.back().second==8);
    sequence.add(0x04,6,8);sequence.cancel();sequence.tick(7500000,8,send);
    assert(sequence.idle()&&sent.size()==18);
    sequence.add(0x04,0,8);
    assert(sequence.idle());
}

static void volume_tests() {
    amity::KeySequence volume(100);
    std::vector<std::pair<uint8_t, uint32_t>> accepted;
    bool admit = true;
    const auto send = [&](uint8_t key, uint32_t generation) {
        if (admit) accepted.emplace_back(key, generation);
        return admit;
    };
    volume.add(0x41, 100, 7);
    volume.tick(250000, 7, send); volume.tick(500000, 7, send);
    assert(accepted.size() == 2 && !volume.idle());
    // Changing activity or its configuration cancels the 98 remaining steps.
    volume.tick(750000, 8, send);
    assert(accepted.size() == 2 && volume.idle());
    // Older adjustments already waiting in the application queue are stale,
    // even when a later activity happens to reuse the same input index.
    volume.add(0x41, 50, 7); volume.tick(1000000, 9, send);
    assert(accepted.size() == 2 && volume.idle());
    volume.add(0x42, 2, 9);
    admit = false; volume.tick(1250000, 9, send);
    assert(!volume.idle() && accepted.size() == 2);
    admit = true; volume.tick(1499999, 9, send); assert(accepted.size() == 2);
    volume.tick(1500000, 9, send); assert(!volume.idle());
    volume.tick(1750000, 9, send); assert(volume.idle());
    assert(accepted.size() == 4 && accepted[2] == std::make_pair(uint8_t{0x42}, uint32_t{9}) && accepted[3] == accepted[2]);
}

int main() {
    swipe_tests();
    volume_tests();
    puts("Paced swipe and volume steps: coalescing, bounded counts, admission retry and activity cancellation tests passed");
}
