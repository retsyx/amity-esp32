/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The CHIP task's work queue and stack lock. The Amity build tracks the stack
// lock (CHIP_STACK_LOCK_TRACKING_ENABLED), so a thread can ask whether it
// holds it. Scheduled work runs as the CHIP task, holding the stack lock, when
// a scenario runs it, or at once (amity_fake::chip_work).
#include "lib/core/CHIPError.h"
#include <cstdint>

namespace chip {
namespace DeviceLayer {

typedef void (*AsyncWorkFunct)(intptr_t arg);

class PlatformManager {
public:
    CHIP_ERROR ScheduleWork(AsyncWorkFunct workFunct, intptr_t arg = 0);
    bool IsChipStackLockedByCurrentThread() const;
};

PlatformManager &PlatformMgr();

}  // namespace DeviceLayer
}  // namespace chip
