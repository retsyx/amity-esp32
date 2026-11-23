/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the CHIP fakes: the CHIP task, which runs scheduled
// work and System Layer timers holding the stack lock, and the stack lock
// itself. The SDK calls that answer questions (fabrics, fail-safe, the Wi-Fi
// driver, BLE advertising, esp_netif, esp_wifi) are strict seams. Each
// scenario runs in its own process, so nothing here needs resetting.
#include "lib/core/CHIPError.h"
#include "system/SystemLayer.h"
#include <cstddef>
#include <functional>
#include <vector>

namespace amity_fake {

// How PlatformMgr().ScheduleWork runs work. queued (the default): work waits
// until the scenario runs it. immediate: the scheduling thread runs it as the
// CHIP task before ScheduleWork returns; it must not hold the stack lock.
enum class ChipWork { queued, immediate };
void chip_work(ChipWork mode);
// The next count ScheduleWork calls fail with error instead of accepting work.
void fail_chip_scheduling(unsigned count, CHIP_ERROR error = CHIP_ERROR_NO_MEMORY);
// Work accepted by ScheduleWork, and the part of it not yet run.
unsigned scheduled_chip_work();
std::size_t queued_chip_work();
// Runs the oldest queued work as the CHIP task; false when none is queued.
bool run_chip_work();
// Runs queued work, including work it queues, until none is left; returns how many ran.
std::size_t run_all_chip_work();

// Runs fn on this thread as the CHIP task, holding the stack lock.
void run_as_chip_task(const std::function<void()> &fn);
bool on_chip_task();
// Whether the calling thread holds the stack lock.
bool chip_stack_locked();
// Runs each time a ScopedChipStackLock takes the stack lock, on the taking
// thread before it holds the lock, so a scenario can check lock order.
void on_chip_stack_lock(std::function<void()> hook);
// How many times a ScopedChipStackLock took the stack lock.
unsigned chip_stack_locks();

struct ChipTimer {
    chip::System::Clock::Timeout delay;
    chip::System::TimerCompleteCallback callback;
    void *context;
};
// Pending System Layer timers, in start order.
std::vector<ChipTimer> chip_timers();
// How many times CancelTimer was called, whether or not a timer matched.
unsigned chip_timer_cancels();
// Fires the oldest pending timer as the CHIP task; false when none is pending.
bool fire_chip_timer();

}  // namespace amity_fake
