/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the core SDK fakes: the scripted clock, recorded
// tasks, queues and logs, and the typed exceptions that stand in for an
// ESP-IDF restart or abort. Each scenario runs in its own process, so nothing
// here needs resetting.
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace amity_fake {

// The scripted clock, in microseconds since boot. Advancing it fires due
// esp_timers in deadline order on the advancing thread.
int64_t now_us();
void advance_us(int64_t microseconds);
void advance_ms(int64_t milliseconds);
// How many times esp_timer_get_time has read the clock.
unsigned clock_reads();

// The next count esp_timer_create calls fail with ESP_ERR_NO_MEM, or
// esp_timer_start calls with ESP_FAIL. A scenario fails if any scripted failure
// here or below is left unused when it returns.
void fail_timer_creations(unsigned count);
void fail_timer_starts(unsigned count);
// Timers created and not yet deleted.
std::size_t live_timers();
struct Timer {
    std::string name;
    bool armed, deleted;
    // The latest deadline the timer was started or restarted with.
    int64_t deadline_us;
    uint64_t period_us;
    unsigned starts;
};
// Created timers, in creation order.
std::vector<Timer> timers();

// Thrown by an on_delay hook to leave a task's endless loop; run_task()
// catches it and marks the task left.
struct LeaveTask {};
// Runs at each vTaskDelay, before the clock advances; it may throw LeaveTask.
void on_delay(std::function<void(TickType_t)> hook);
unsigned delays();

// How a FreeRTOS wait that cannot succeed at once behaves. advance (the
// default, for scenarios on one thread): time passes, firing timers, until the
// wait succeeds or times out, and an endless wait that nothing could end fails
// the scenario. block (for scenarios that run tasks on threads): the wait
// blocks until another thread satisfies it or advances the clock past its
// deadline.
enum class Waits { advance, block };
void waits(Waits mode);

struct Task {
    TaskHandle_t handle;
    std::string name;
    TaskFunction_t entry;
    void *argument;
    uint32_t stack_bytes;
    UBaseType_t priority;
    BaseType_t core;
    UBaseType_t caps;  // 0 unless created with capabilities
    bool with_caps;
    // created: not started; running; deleted: deleted itself or by another
    // task; left: ended by LeaveTask.
    enum class State { created, running, deleted, left } state;
};
// Successfully created tasks, in creation order.
std::vector<Task> tasks();
// The next count task creations fail.
void fail_task_creations(unsigned count);
// Runs a created task on this thread until it deletes itself or leaves.
void run_task(TaskHandle_t handle);
// Runs a created task on a new thread; join it before the scenario ends.
std::thread start_task(TaskHandle_t handle);

struct Queue {
    QueueHandle_t handle;
    enum class Kind { queue, binary, counting, mutex, recursive_mutex } kind;
    UBaseType_t length, item_size;
    bool is_static, with_caps, deleted;
};
// Created queues and semaphores, in creation order.
std::vector<Queue> queues();
// The next count queue or semaphore creations fail.
void fail_queue_creations(unsigned count);

// What xPortInIsrContext reports.
void in_isr(bool value);

// Thrown by esp_restart.
struct Restart {};
// Thrown where ESP_ERROR_CHECK would abort.
struct ErrorCheckFailed {
    esp_err_t error;
    std::string expression;
};
std::vector<shutdown_handler_t> shutdown_handlers();

struct LogLine {
    esp_log_level_t level;
    std::string tag, message;
};
std::vector<LogLine> log_lines();
// Whether any recorded message contains text.
bool logged(std::string_view text);
// Runs on the logging thread for each line, after it is recorded, so a
// scenario can check which locks the logging code holds.
void on_log(std::function<void(const LogLine &)> hook);

// The next count heap_caps allocations fail and call the failed-allocation hook.
void fail_allocations(unsigned count);
// The capabilities a live heap_caps allocation was made with; 0 for any other pointer.
uint32_t allocation_caps(const void *pointer);
std::size_t live_allocations();
std::size_t extmem_limit();

// The descriptor esp_app_get_description returns.
esp_app_desc_t &app_description();

}  // namespace amity_fake
