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
// Owner-task pacing for finite key steps: Siri swipe navigation and Matter's
// relative volume. Steps belong to the activity generation that accepted them,
// including a later restart of the same activity index. The hub sends each step
// as a complete press/release and checks the generation again on dequeue.
class KeySequence {
public:
    // Repeated steps of a pending key merge, up to `limit` presses.
    explicit KeySequence(uint8_t limit) : limit_(limit) {}
    bool idle() const { return size_ == 0; }
    void cancel() { size_ = 0; }

    void add(uint8_t key, uint8_t count, uint32_t activity_generation) {
        if (!count) return;
        if (generation_ != activity_generation) cancel();
        generation_ = activity_generation;
        for (uint8_t i = 0; i < size_; ++i) {
            if (pending_[i].key != key) continue;
            const unsigned total = pending_[i].count + count;
            pending_[i].count = static_cast<uint8_t>(total > limit_ ? limit_ : total);
            return;
        }
        if (size_ == pending_.size()) return;
        pending_[size_++] = Step{key, count > limit_ ? limit_ : count};
    }

    template <typename Sender>
    void tick(uint64_t now_us, uint32_t current_generation, Sender send) {
        if (generation_ != current_generation) cancel();
        // A complete CEC press/release needs about 165 ms of clear bus time.
        // Leave room for replies.
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
    // Swipes have four directions; volume uses one key at a time.
    std::array<Step, 4> pending_{};
    uint8_t size_ = 0, limit_;
    uint32_t generation_ = 0;
    uint64_t last_attempt_us_ = 0;
};
}
