/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "pairing_policy.hpp"
#include <cassert>
#include <cstdio>

int main() {
    using ble::pairing_candidate;
    assert(pairing_candidate(true, true, false, -50, -50));
    assert(pairing_candidate(true, true, false, -35, -50));
    assert(!pairing_candidate(true, true, false, -51, -50));
    assert(!pairing_candidate(false, true, false, -30, -50));
    assert(!pairing_candidate(true, false, false, -30, -50));
    assert(!pairing_candidate(true, true, true, -30, -50));
    assert(pairing_candidate(true, true, false, -70, -75));
    puts("Remote pairing RSSI boundary, HID, connectability and rejected-peer tests passed");
}
