/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "atomic_remote.hpp"

namespace ble {
namespace {
struct AtomicKey {
    uint16_t page, usage;
    RemoteButton button;
};
// CouchClickerAtomic boards/shields/cca/cca.keymap, commit
// 11b37040123b897e8d591d63446d87d770a79233. No d-pad activity aliases.
// Consumer usages support equivalent standard-keyboard firmware builds.
constexpr AtomicKey keys[] = {
    {0x07, 0x28, RemoteButton::Select},    // Enter
    {0x07, 0x29, RemoteButton::Back},      // Escape
    {0x07, 0x3a, RemoteButton::Activity0}, // F1-F4
    {0x07, 0x3b, RemoteButton::Activity1},
    {0x07, 0x3c, RemoteButton::Activity2},
    {0x07, 0x3d, RemoteButton::Activity3},
    {0x07, 0x4a, RemoteButton::Home},
    {0x07, 0x4f, RemoteButton::Right},
    {0x07, 0x50, RemoteButton::Left},
    {0x07, 0x51, RemoteButton::Down},
    {0x07, 0x52, RemoteButton::Up},
    {0x07, 0x66, RemoteButton::Power},     // Keyboard Power
    {0x07, 0x7f, RemoteButton::Mute},
    {0x07, 0x80, RemoteButton::VolumeUp},
    {0x07, 0x81, RemoteButton::VolumeDown},
    {0x0c, 0x30, RemoteButton::Power},
    {0x0c, 0x41, RemoteButton::Select},
    {0x0c, 0x42, RemoteButton::Up},
    {0x0c, 0x43, RemoteButton::Down},
    {0x0c, 0x44, RemoteButton::Left},
    {0x0c, 0x45, RemoteButton::Right},
    {0x0c, 0x46, RemoteButton::Back},
    {0x0c, 0x224, RemoteButton::Back},
    {0x0c, 0x223, RemoteButton::Home},
    {0x0c, 0xe2, RemoteButton::Mute},
    {0x0c, 0xe9, RemoteButton::VolumeUp},
    {0x0c, 0xea, RemoteButton::VolumeDown},
};
} // namespace

std::optional<RemoteButton> atomic_button(uint16_t page, uint16_t usage) {
    for (const auto &key : keys)
        if (key.page == page && key.usage == usage) return key.button;
    return std::nullopt;
}

void AtomicInputAdapter::input(uint16_t page, uint16_t usage, bool pressed, InputStamp stamp) {
    const auto button = atomic_button(page, usage);
    if (!button) return;
    const ButtonEvent event{(static_cast<uint32_t>(page) << 16) | usage, *button, pressed};
    controller_.buttons(RemoteProfile::Atomic, &event, 1, stamp);
}
} // namespace ble
