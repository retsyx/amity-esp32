/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "atomic_remote.h"
#include <array>
#include <cstdint>

namespace ble {
enum class AtomicCommandType { Press, Release, Activity, PowerShort, PowerLong };
struct AtomicCommand { AtomicCommandType type; uint8_t value = 0; };

// Portable press/hold policy, separate from both the HID transport and the hub.
// Time is monotonic microseconds. One owner task calls input(), tick(), cancel().
class AtomicController {
public:
    using Callback = void (*)(AtomicCommand command, void *context);
    explicit AtomicController(Callback callback, void *context = nullptr, uint8_t long_activity_offset = 4)
        : callback_(callback), context_(context), long_activity_offset_(long_activity_offset) {}
    void input(uint16_t page, uint16_t usage, bool pressed, uint64_t now);
    // Link loss and event queue overflow cancel pending power/activity actions.
    void cancel();
    static constexpr uint64_t long_press_us = 500000;
private:
    struct Held {
        bool down = false, suppressed = false;
        uint16_t page = 0, usage = 0;
        uint64_t started = 0, order = 0;
        atomic_action_t action{};
    };
    std::array<Held, 32> held_{};
    Callback callback_;
    void *context_;
    uint64_t next_order_ = 0;
    int current_key_ = -1;
    uint8_t long_activity_offset_;
    void emit(AtomicCommandType type, uint8_t value = 0);
    void reconcile();
};
} // namespace ble
