/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// ESP Matter's scoped CHIP stack lock. With stack lock tracking, as in the
// Amity build, a thread that already holds the lock neither takes nor
// releases it again.
#include <cstdint>

namespace esp_matter {
namespace lock {

class ScopedChipStackLock {
public:
    explicit ScopedChipStackLock(uint32_t ticks_to_wait);
    ~ScopedChipStackLock();
    ScopedChipStackLock(const ScopedChipStackLock &) = delete;
    ScopedChipStackLock &operator=(const ScopedChipStackLock &) = delete;

private:
    bool locked;
};

}  // namespace lock
}  // namespace esp_matter
