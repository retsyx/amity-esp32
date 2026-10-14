/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */
#include "atomic_controller.hpp"
#include "atomic_remote.h"

namespace ble {
void AtomicInputAdapter::input(uint16_t page, uint16_t usage, bool pressed, InputStamp stamp) {
    atomic_action_t action;
    if (!atomic_remote_translate(page, usage, &action)) return;
    const auto button = action.type == ATOMIC_ACTIVITY
        ? static_cast<RemoteButton>(static_cast<uint16_t>(RemoteButton::Activity0) + action.value)
        : static_cast<RemoteButton>(action.value);
    const ButtonEvent event{(static_cast<uint32_t>(page) << 16) | usage, button, pressed};
    controller_.buttons(RemoteProfile::Atomic, &event, 1, stamp);
}
} // namespace ble
