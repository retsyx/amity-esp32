/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "atomic_remote.h"

// CouchClickerAtomic boards/shields/cca/cca.keymap, commit
// 11b37040123b897e8d591d63446d87d770a79233. No d-pad activity aliases.
bool atomic_remote_translate(uint16_t page, uint16_t usage, atomic_action_t *out) {
    if (!out) return false;
    out->type = ATOMIC_CEC_KEY;
    if (page == 7) {
        if (usage >= 0x3a && usage <= 0x3d) {
            out->type = ATOMIC_ACTIVITY;
            out->value = usage - 0x3a;
            return true;
        }
        switch (usage) {
        case 0x28: out->value = 0x00; return true; // Enter -> Select
        case 0x29: out->value = 0x0d; return true; // Escape -> Back
        case 0x4a: out->value = 0x09; return true; // Home -> Root Menu
        case 0x4f: out->value = 0x04; return true;
        case 0x50: out->value = 0x03; return true;
        case 0x51: out->value = 0x02; return true;
        case 0x52: out->value = 0x01; return true;
        case 0x66: out->value = 0x40; return true; // Keyboard Power
        case 0x7f: out->value = 0x43; return true;
        case 0x80: out->value = 0x41; return true;
        case 0x81: out->value = 0x42; return true;
        default: return false;
        }
    }
    // Consumer usages support equivalent standard-keyboard firmware builds.
    if (page == 0x0c) {
        switch (usage) {
        case 0x30: out->value = 0x40; return true;
        case 0x41: out->value = 0x00; return true;
        case 0x42: out->value = 0x01; return true;
        case 0x43: out->value = 0x02; return true;
        case 0x44: out->value = 0x03; return true;
        case 0x45: out->value = 0x04; return true;
        case 0x46: case 0x224: out->value = 0x0d; return true;
        case 0x223: out->value = 0x09; return true;
        case 0xe2: out->value = 0x43; return true;
        case 0xe9: out->value = 0x41; return true;
        case 0xea: out->value = 0x42; return true;
        default: return false;
        }
    }
    return false;
}
