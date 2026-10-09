/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <array>
#include <cstdint>

namespace amity {
// Swipe navigation is finite and belongs to the activity that received it.
// The hub sends each admitted step as a complete press/release and checks the
// captured generation again when consuming it on its owner task.
class SwipeSequence {
public:
    bool idle() const { return size_ == 0; }
    void cancel() { size_ = 0; }

    void add(uint8_t key, uint8_t count, uint32_t activity_generation) {
        if (key < 0x01 || key > 0x04 || !count) return;
        if (generation_ != activity_generation) cancel();
        generation_ = activity_generation;
        // Python merges repeat-counted presses of the same key, capped at ten.
        for (uint8_t i = 0; i < size_; ++i) {
            if (pending_[i].key != key) continue;
            const unsigned total = pending_[i].count + count;
            pending_[i].count = total > 10 ? 10 : total;
            return;
        }
        // There are only four swipe directions, each occupying at most a slot.
        pending_[size_++] = Step{key, static_cast<uint8_t>(count > 10 ? 10 : count)};
    }

    template <typename Sender>
    void tick(uint64_t now_us, uint32_t current_generation, Sender send) {
        if (generation_ != current_generation) cancel();
        if (idle() || now_us - last_attempt_us_ < 250000) return;
        // Keep a rejected admission pending, without building a CEC backlog.
        if (send(pending_[0].key, generation_) && --pending_[0].count == 0) {
            for (uint8_t i = 1; i < size_; ++i) pending_[i - 1] = pending_[i];
            --size_;
        }
        last_attempt_us_ = now_us;
    }
private:
    struct Step { uint8_t key, count; };
    std::array<Step, 4> pending_{};
    uint8_t size_ = 0;
    uint32_t generation_ = 0;
    uint64_t last_attempt_us_ = 0;
};
}
