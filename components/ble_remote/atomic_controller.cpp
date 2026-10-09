/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "atomic_controller.hpp"

namespace ble {
void AtomicController::emit(AtomicCommandType type, uint8_t value) {
    if (callback_) callback_(AtomicCommand{type, value}, context_);
}

void AtomicController::reconcile() {
    const Held *latest = nullptr;
    for (const auto &key : held_)
        if (key.down && !key.suppressed && key.action.type == ATOMIC_CEC_KEY &&
            key.action.value != 0x40 && (!latest || latest->order < key.order)) latest = &key;
    const int next = latest ? latest->action.value : -1;
    if (next == current_key_) return; // Two HID usages may represent the same CEC key.
    if (current_key_ >= 0) emit(AtomicCommandType::Release);
    current_key_ = next;
    if (current_key_ >= 0) {
        emit(AtomicCommandType::Press, static_cast<uint8_t>(current_key_));
    }
}

void AtomicController::input(uint16_t page, uint16_t usage, bool pressed, uint64_t now) {
    atomic_action_t action;
    if (!atomic_remote_translate(page, usage, &action)) return;
    Held *key = nullptr, *empty = nullptr;
    for (auto &entry : held_) {
        if (entry.down && entry.page == page && entry.usage == usage) key = &entry;
        if (!entry.down && !empty) empty = &entry;
    }
    if (pressed) {
        if (key) return;
        if (!empty) { cancel(); return; }
        *empty = Held{true, false, page, usage, now, ++next_order_, action};
    } else {
        if (!key) return;
        const bool suppressed = key->suppressed;
        const bool long_press = now >= key->started && now - key->started >= long_press_us;
        key->down = false;
        if (!suppressed && (action.type == ATOMIC_ACTIVITY || action.value == 0x40)) {
            // Do not leak held navigation/audio keys into the next activity.
            for (auto &entry : held_) if (entry.down) entry.suppressed = true;
            reconcile();
            if (action.type == ATOMIC_ACTIVITY)
                emit(AtomicCommandType::Activity, action.value + (long_press ? long_activity_offset_ : 0));
            else emit(long_press ? AtomicCommandType::PowerLong : AtomicCommandType::PowerShort);
        }
    }
    reconcile();
}

void AtomicController::cancel() {
    for (auto &entry : held_) entry = {};
    if (current_key_ >= 0) emit(AtomicCommandType::Release);
    current_key_ = -1;
}
} // namespace ble
