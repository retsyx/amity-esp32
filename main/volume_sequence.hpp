/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <cstdint>

namespace amity {
struct VolumeAdjustment {
    int16_t steps;
    uint32_t activity_generation;
};

// Owner-task pacing for Matter's relative volume commands. A command belongs
// to the activity/configuration that accepted it, including a later restart
// of the same activity index. The hub checks this generation again on dequeue.
class VolumeSequence {
public:
    bool idle() const { return pending_.steps == 0; }
    void start(VolumeAdjustment adjustment) { pending_ = adjustment; }

    template <typename Sender>
    void tick(uint64_t now_us, uint32_t current_generation, Sender send) {
        if (pending_.activity_generation != current_generation) pending_.steps = 0;
        if (idle() || now_us - last_attempt_us_ < 250000) return;
        const bool up = pending_.steps > 0;
        // Rejected admission leaves the step pending for the next paced try.
        if (send(up ? 0x41 : 0x42, pending_.activity_generation)) pending_.steps += up ? -1 : 1;
        last_attempt_us_ = now_us;
    }
private:
    VolumeAdjustment pending_{};
    uint64_t last_attempt_us_ = 0;
};
}
