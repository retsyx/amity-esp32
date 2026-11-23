/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The System Layer's one-shot timers, as the pinned FreeRTOS layer runs them:
// both calls need the stack lock, and starting a timer replaces the pending
// one with the same callback and context. Timers fire on the CHIP task when a
// scenario fires them (amity_fake::fire_chip_timer).
#include "lib/core/CriticalFailure.h"
#include "system/SystemClock.h"

namespace chip {
namespace System {

class Layer;
using TimerCompleteCallback = void (*)(Layer *aLayer, void *appState);

class Layer {
public:
    CriticalFailure StartTimer(Clock::Timeout aDelay, TimerCompleteCallback aComplete, void *aAppState);
    void CancelTimer(TimerCompleteCallback aOnComplete, void *aAppState);
};

}  // namespace System
}  // namespace chip
