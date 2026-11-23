/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The core SDK fakes. One lock guards every queue, task and timer, so waits
// and the scripted clock stay consistent across threads.
#include "core_fakes.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "seams.hpp"
#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>

struct tskTaskControlBlock {
    amity_fake::Task task;
    UBaseType_t notifications = 0;
};

struct QueueDefinition {
    std::size_t record = 0;  // index into the queue records
    amity_fake::Queue::Kind kind = amity_fake::Queue::Kind::queue;
    UBaseType_t length = 0, item_size = 0;
    uint8_t *storage = nullptr;  // the ring, for item queues
    bool owns_storage = false, is_static = false;
    UBaseType_t head = 0, count = 0;  // items, or the semaphore's count
    std::thread::id holder;           // the thread holding a mutex
    TaskHandle_t holder_task = nullptr;
    UBaseType_t depth = 0;  // recursive mutex depth
};
static_assert(sizeof(QueueDefinition) <= sizeof(StaticQueue_t), "StaticQueue_t holds a queue");
static_assert(alignof(QueueDefinition) <= alignof(StaticQueue_t), "StaticQueue_t aligns a queue");

struct esp_timer {
    esp_timer_create_args_t args{};
    bool armed = false, running = false, deleted = false;
    int64_t deadline = 0;
    uint64_t period = 0;
    unsigned starts = 0;
    std::thread::id runner;
};

namespace {

using amity_fake::Queue;
using amity_fake::Task;

[[noreturn]] void fail(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

std::mutex state;
std::condition_variable changed;
std::atomic<int64_t> clock_us{0};
std::atomic<unsigned> clock_read_count{0};
amity_fake::Waits wait_mode = amity_fake::Waits::advance;
std::function<void(TickType_t)> delay_hook;
unsigned delay_count = 0;
std::deque<std::unique_ptr<tskTaskControlBlock>> task_blocks;
unsigned task_failures = 0;
std::vector<Queue> queue_records;
unsigned queue_failures = 0;
std::deque<std::unique_ptr<esp_timer>> timer_blocks;
unsigned timer_creation_failures = 0, timer_start_failures = 0;
std::atomic<bool> isr{false};
thread_local tskTaskControlBlock *current_task = nullptr;

// The calling thread's task: the recorded task it runs, or an implicit main task.
tskTaskControlBlock *self() {
    if (current_task) return current_task;
    thread_local tskTaskControlBlock implicit{Task{nullptr, "main", nullptr, nullptr, 0, 0, tskNO_AFFINITY, 0, false,
                                                   Task::State::running}};
    implicit.task.handle = &implicit;
    return &implicit;
}

int64_t ticks_us(TickType_t ticks) { return int64_t(ticks) * portTICK_PERIOD_MS * 1000; }

esp_timer *next_due(int64_t limit) {
    esp_timer *next = nullptr;
    for (const auto &timer : timer_blocks) {
        if (timer->armed && !timer->deleted && timer->deadline <= limit && (!next || timer->deadline < next->deadline))
            next = timer.get();
    }
    return next;
}

// Fires one timer with the state lock held, releasing it during the callback.
void fire(std::unique_lock<std::mutex> &lock, esp_timer *timer) {
    if (clock_us.load() < timer->deadline) clock_us.store(timer->deadline);
    if (timer->period)
        timer->deadline += int64_t(timer->period);
    else
        timer->armed = false;
    timer->running = true;
    timer->runner = std::this_thread::get_id();
    const auto callback = timer->args.callback;
    void *argument = timer->args.arg;
    lock.unlock();
    callback(argument);
    lock.lock();
    timer->running = false;
    changed.notify_all();
}

void advance_to(std::unique_lock<std::mutex> &lock, int64_t target) {
    while (esp_timer *timer = next_due(target)) fire(lock, timer);
    if (clock_us.load() < target) clock_us.store(target);
    changed.notify_all();
}

// Waits with the state lock held until ready() or the wait times out; see Waits.
template <typename Ready> bool wait(std::unique_lock<std::mutex> &lock, TickType_t ticks, const char *what, Ready ready) {
    if (ready()) return true;
    if (ticks == 0) return false;
    const bool forever = ticks == portMAX_DELAY;
    const int64_t deadline = forever ? INT64_MAX : clock_us.load() + ticks_us(ticks);
    if (wait_mode == amity_fake::Waits::advance) {
        for (unsigned fired = 0; esp_timer *timer = next_due(deadline); ++fired) {
            if (forever && fired == 100000) break;
            fire(lock, timer);
            if (ready()) return true;
        }
        if (forever) fail(std::string("an endless wait on ") + what + " that nothing in this thread can end");
        if (clock_us.load() < deadline) clock_us.store(deadline);
        changed.notify_all();
        return ready();
    }
    // The predicate performs the operation when it can, so it is evaluated
    // once per wake-up and its last result is the outcome.
    bool done = false;
    changed.wait(lock, [&] {
        done = ready();
        return done || clock_us.load() >= deadline;
    });
    return done;
}

QueueDefinition *create_queue(Queue::Kind kind, UBaseType_t length, UBaseType_t item_size, uint8_t *storage,
                              StaticQueue_t *control, bool with_caps) {
    std::lock_guard<std::mutex> lock(state);
    if (queue_failures) {
        --queue_failures;
        return nullptr;
    }
    if (control && kind == Queue::Kind::queue && item_size && !storage) fail("a static queue without storage");
    auto *queue = control ? new (control) QueueDefinition : new QueueDefinition;
    queue->kind = kind;
    queue->length = length;
    queue->item_size = kind == Queue::Kind::queue ? item_size : 0;
    queue->is_static = control != nullptr;
    if (queue->item_size) {
        queue->storage = storage ? storage : new uint8_t[std::size_t(length) * item_size];
        queue->owns_storage = !storage;
    }
    queue->record = queue_records.size();
    queue_records.push_back({queue, kind, length, queue->item_size, queue->is_static, with_caps, false});
    return queue;
}

void check_live(QueueHandle_t queue) {
    if (!queue) fail("a FreeRTOS call on a null queue or semaphore");
    if (queue_records.at(queue->record).deleted) fail("a FreeRTOS call on a deleted queue or semaphore");
}

bool is_mutex(QueueHandle_t queue) {
    return queue->kind == Queue::Kind::mutex || queue->kind == Queue::Kind::recursive_mutex;
}

// Takes one count or item; the caller holds the state lock.
bool receive_locked(QueueHandle_t queue, void *buffer, bool remove) {
    if (!queue->count) return false;
    if (queue->item_size) {
        std::memcpy(buffer, queue->storage + std::size_t(queue->head) * queue->item_size, queue->item_size);
        if (!remove) return true;
        queue->head = (queue->head + 1) % queue->length;
    }
    if (remove) --queue->count;
    if (remove && is_mutex(queue)) {
        queue->holder = std::this_thread::get_id();
        queue->holder_task = self()->task.handle;
        queue->depth = 1;
    }
    changed.notify_all();
    return true;
}

bool send_locked(QueueHandle_t queue, const void *item, BaseType_t position) {
    if (position == queueOVERWRITE && queue->length == 1 && queue->count == 1) queue->count = 0;
    if (queue->count >= queue->length) return false;
    if (queue->item_size) {
        UBaseType_t slot;
        if (position == queueSEND_TO_FRONT) {
            queue->head = (queue->head + queue->length - 1) % queue->length;
            slot = queue->head;
        } else {
            slot = (queue->head + queue->count) % queue->length;
        }
        std::memcpy(queue->storage + std::size_t(slot) * queue->item_size, item, queue->item_size);
    }
    ++queue->count;
    changed.notify_all();
    return true;
}

BaseType_t give_locked(QueueHandle_t queue) {
    if (is_mutex(queue)) {
        if (queue->count || queue->holder != std::this_thread::get_id()) return pdFALSE;
        queue->holder = {};
        queue->holder_task = nullptr;
        queue->depth = 0;
    }
    return send_locked(queue, nullptr, queueSEND_TO_BACK) ? pdTRUE : pdFALSE;
}

BaseType_t create_task(TaskFunction_t entry, const char *name, uint32_t stack, void *argument, UBaseType_t priority,
                       TaskHandle_t *created, BaseType_t core, UBaseType_t caps, bool with_caps) {
    std::lock_guard<std::mutex> lock(state);
    if (task_failures) {
        --task_failures;
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    if (!entry) fail("xTaskCreate without a task function");
    task_blocks.push_back(std::make_unique<tskTaskControlBlock>());
    auto *block = task_blocks.back().get();
    block->task = Task{block, name ? name : "", entry, argument, stack, priority, core, caps, with_caps,
                       Task::State::created};
    if (created) *created = block;
    return pdPASS;
}

struct SelfDeleted {};

void delete_task(TaskHandle_t handle, bool with_caps) {
    tskTaskControlBlock *block = handle ? handle : self();
    std::unique_lock<std::mutex> lock(state);
    if (!block->task.entry) fail("vTaskDelete of a task the scenario did not create, or of the main task");
    if (block->task.with_caps != with_caps)
        fail("task " + block->task.name + (with_caps ? " was created without capabilities but deleted with vTaskDeleteWithCaps"
                                                     : " was created with capabilities but deleted with vTaskDelete"));
    if (block->task.state == Task::State::deleted) fail("task " + block->task.name + " was deleted twice");
    block->task.state = Task::State::deleted;
    changed.notify_all();
    if (block == current_task) {
        lock.unlock();
        throw SelfDeleted{};
    }
}

struct LogState {
    std::mutex mutex;
    std::vector<amity_fake::LogLine> lines;
    std::function<void(const amity_fake::LogLine &)> hook;
    vprintf_like_t sink = nullptr;
};
LogState &logs() {
    static LogState instance;
    return instance;
}

int call_sink(vprintf_like_t sink, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = sink(format, arguments);
    va_end(arguments);
    return result;
}

struct Heap {
    std::mutex mutex;
    std::map<const void *, uint32_t> live;
    unsigned failures = 0;
    esp_alloc_failed_hook_t failed_hook = nullptr;
    std::size_t extmem_limit = 0;
};
Heap &heap() {
    static Heap instance;
    return instance;
}

bool allocation_fails(std::size_t size, uint32_t caps, const char *function) {
    esp_alloc_failed_hook_t hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(heap().mutex);
        if (!heap().failures) return false;
        --heap().failures;
        hook = heap().failed_hook;
    }
    if (hook) hook(size, caps, function);
    return true;
}

void *remember(void *pointer, uint32_t caps) {
    if (pointer) {
        std::lock_guard<std::mutex> lock(heap().mutex);
        heap().live[pointer] = caps;
    }
    return pointer;
}

std::recursive_mutex critical;

esp_app_desc_t description = [] {
    esp_app_desc_t value{};
    value.magic_word = ESP_APP_DESC_MAGIC_WORD;
    std::strcpy(value.version, "0.0.0-test");
    std::strcpy(value.project_name, "amity");
    std::strcpy(value.idf_ver, "v6.1");
    return value;
}();

std::mutex shutdown_mutex;
std::vector<shutdown_handler_t> shutdown_list;

}  // namespace

namespace amity_fake {

int64_t now_us() { return clock_us.load(); }
void advance_us(int64_t microseconds) {
    if (microseconds < 0) fail("the scripted clock cannot go back");
    std::unique_lock<std::mutex> lock(state);
    advance_to(lock, clock_us.load() + microseconds);
}
void advance_ms(int64_t milliseconds) { advance_us(milliseconds * 1000); }
unsigned clock_reads() { return clock_read_count.load(); }

void fail_timer_creations(unsigned count) {
    std::lock_guard<std::mutex> lock(state);
    timer_creation_failures = count;
}
void fail_timer_starts(unsigned count) {
    std::lock_guard<std::mutex> lock(state);
    timer_start_failures = count;
}
std::size_t live_timers() {
    std::lock_guard<std::mutex> lock(state);
    return std::size_t(std::count_if(timer_blocks.begin(), timer_blocks.end(),
                                     [](const std::unique_ptr<esp_timer> &timer) { return !timer->deleted; }));
}

std::vector<Timer> timers() {
    std::lock_guard<std::mutex> lock(state);
    std::vector<Timer> result;
    for (const auto &timer : timer_blocks)
        result.push_back({timer->args.name ? timer->args.name : "", timer->armed, timer->deleted, timer->deadline,
                          timer->period, timer->starts});
    return result;
}

void on_delay(std::function<void(TickType_t)> hook) {
    std::lock_guard<std::mutex> lock(state);
    delay_hook = std::move(hook);
}
unsigned delays() {
    std::lock_guard<std::mutex> lock(state);
    return delay_count;
}
void waits(Waits mode) {
    std::lock_guard<std::mutex> lock(state);
    wait_mode = mode;
}

std::vector<Task> tasks() {
    std::lock_guard<std::mutex> lock(state);
    std::vector<Task> result;
    for (const auto &block : task_blocks) result.push_back(block->task);
    return result;
}
void fail_task_creations(unsigned count) {
    std::lock_guard<std::mutex> lock(state);
    task_failures = count;
}
void run_task(TaskHandle_t handle) {
    {
        std::lock_guard<std::mutex> lock(state);
        if (!handle || !handle->task.entry) fail("run_task needs a created task");
        if (handle->task.state != Task::State::created) fail("task " + handle->task.name + " already ran");
        handle->task.state = Task::State::running;
    }
    tskTaskControlBlock *previous = current_task;
    current_task = handle;
    try {
        handle->task.entry(handle->task.argument);
    } catch (const SelfDeleted &) {
        current_task = previous;
        return;
    } catch (const LeaveTask &) {
        current_task = previous;
        std::lock_guard<std::mutex> lock(state);
        handle->task.state = Task::State::left;
        changed.notify_all();
        return;
    } catch (...) {
        current_task = previous;
        throw;
    }
    current_task = previous;
    fail("task " + handle->task.name + " returned from its function without deleting itself");
}
std::thread start_task(TaskHandle_t handle) { return std::thread([handle] { run_task(handle); }); }

std::vector<Queue> queues() {
    std::lock_guard<std::mutex> lock(state);
    return queue_records;
}
void fail_queue_creations(unsigned count) {
    std::lock_guard<std::mutex> lock(state);
    queue_failures = count;
}

void in_isr(bool value) { isr.store(value); }

std::vector<shutdown_handler_t> shutdown_handlers() {
    std::lock_guard<std::mutex> lock(shutdown_mutex);
    return shutdown_list;
}

std::vector<LogLine> log_lines() {
    std::lock_guard<std::mutex> lock(logs().mutex);
    return logs().lines;
}
bool logged(std::string_view text) {
    std::lock_guard<std::mutex> lock(logs().mutex);
    return std::any_of(logs().lines.begin(), logs().lines.end(),
                       [&](const LogLine &line) { return line.message.find(text) != std::string::npos; });
}
void on_log(std::function<void(const LogLine &)> hook) {
    std::lock_guard<std::mutex> lock(logs().mutex);
    logs().hook = std::move(hook);
}

void fail_allocations(unsigned count) {
    std::lock_guard<std::mutex> lock(heap().mutex);
    heap().failures = count;
}
uint32_t allocation_caps(const void *pointer) {
    std::lock_guard<std::mutex> lock(heap().mutex);
    const auto found = heap().live.find(pointer);
    return found == heap().live.end() ? 0 : found->second;
}
std::size_t live_allocations() {
    std::lock_guard<std::mutex> lock(heap().mutex);
    return heap().live.size();
}
std::size_t extmem_limit() {
    std::lock_guard<std::mutex> lock(heap().mutex);
    return heap().extmem_limit;
}

esp_app_desc_t &app_description() { return description; }

}  // namespace amity_fake

extern "C" {

const char *esp_err_to_name(esp_err_t code) {
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_INVALID_VERSION: return "ESP_ERR_INVALID_VERSION";
    case ESP_ERR_INVALID_MAC: return "ESP_ERR_INVALID_MAC";
    case ESP_ERR_NOT_FINISHED: return "ESP_ERR_NOT_FINISHED";
    case ESP_ERR_NOT_ALLOWED: return "ESP_ERR_NOT_ALLOWED";
    default: return "UNKNOWN ERROR";
    }
}
const char *esp_err_to_name_r(esp_err_t code, char *buf, size_t buflen) {
    if (buf && buflen) std::snprintf(buf, buflen, "%s", esp_err_to_name(code));
    return buf;
}
void _esp_error_check_failed(esp_err_t rc, const char *, int, const char *, const char *expression) {
    throw amity_fake::ErrorCheckFailed{rc, expression ? expression : ""};
}

void esp_restart(void) { throw amity_fake::Restart{}; }
esp_err_t esp_register_shutdown_handler(shutdown_handler_t handle) {
    std::lock_guard<std::mutex> lock(shutdown_mutex);
    if (std::find(shutdown_list.begin(), shutdown_list.end(), handle) != shutdown_list.end()) return ESP_ERR_INVALID_STATE;
    if (shutdown_list.size() >= 5) return ESP_ERR_NO_MEM;
    shutdown_list.push_back(handle);
    return ESP_OK;
}
uint32_t esp_get_free_heap_size(void) { return AMITY_SEAM(esp_get_free_heap_size); }
uint32_t esp_get_minimum_free_heap_size(void) { return AMITY_SEAM(esp_get_minimum_free_heap_size); }

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func) {
    std::lock_guard<std::mutex> lock(logs().mutex);
    auto previous = logs().sink ? logs().sink : vprintf;
    logs().sink = func;
    return previous;
}
void esp_log_level_set(const char *, esp_log_level_t) {}
uint32_t esp_log_timestamp(void) { return uint32_t(clock_us.load() / 1000); }
void esp_log_writev(esp_log_level_t level, const char *tag, const char *format, va_list arguments) {
    va_list measure;
    va_copy(measure, arguments);
    const int size = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    std::string message(size > 0 ? std::size_t(size) : 0, '\0');
    if (size > 0) std::vsnprintf(message.data(), message.size() + 1, format, arguments);
    amity_fake::LogLine line{level, tag ? tag : "", message};
    std::function<void(const amity_fake::LogLine &)> hook;
    vprintf_like_t sink;
    {
        std::lock_guard<std::mutex> lock(logs().mutex);
        logs().lines.push_back(line);
        hook = logs().hook;
        sink = logs().sink;
    }
    if (hook) hook(line);
    static const char letters[] = "NEWIDV";
    const char letter = level >= ESP_LOG_NONE && level <= ESP_LOG_VERBOSE ? letters[level] : '?';
    const std::string text = std::string(1, letter) + " (" + std::to_string(esp_log_timestamp()) + ") " + line.tag +
                             ": " + message + "\n";
    if (sink)
        call_sink(sink, "%s", text.c_str());
    else
        std::fputs(text.c_str(), stdout);
}
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    esp_log_writev(level, tag, format, arguments);
    va_end(arguments);
}

int64_t esp_timer_get_time(void) {
    ++clock_read_count;
    return clock_us.load();
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *create_args, esp_timer_handle_t *out_handle) {
    if (!create_args || !create_args->callback || !out_handle) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state);
    if (timer_creation_failures) {
        --timer_creation_failures;
        return ESP_ERR_NO_MEM;
    }
    timer_blocks.push_back(std::make_unique<esp_timer>());
    timer_blocks.back()->args = *create_args;
    *out_handle = timer_blocks.back().get();
    return ESP_OK;
}
static esp_err_t start_timer(esp_timer_handle_t timer, uint64_t first, uint64_t period) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state);
    if (timer->deleted) fail("an esp_timer call on a deleted timer");
    if (timer->armed) return ESP_ERR_INVALID_STATE;
    if (timer_start_failures) {
        --timer_start_failures;
        return ESP_FAIL;
    }
    timer->armed = true;
    timer->deadline = clock_us.load() + int64_t(first);
    timer->period = period;
    ++timer->starts;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) { return start_timer(timer, timeout_us, 0); }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period) { return start_timer(timer, period, period); }
esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state);
    if (!timer->armed) return ESP_ERR_INVALID_STATE;
    timer->deadline = clock_us.load() + int64_t(timeout_us);
    if (timer->period) timer->period = timeout_us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state);
    if (!timer->armed) return ESP_ERR_INVALID_STATE;
    timer->armed = false;
    return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    std::unique_lock<std::mutex> lock(state);
    timer->armed = false;
    if (timer->running && timer->runner != std::this_thread::get_id())
        changed.wait(lock, [&] { return !timer->running; });
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state);
    if (timer->deleted) fail("an esp_timer was deleted twice");
    if (timer->armed) return ESP_ERR_INVALID_STATE;
    timer->deleted = true;
    return ESP_OK;
}
bool esp_timer_is_active(esp_timer_handle_t timer) {
    std::lock_guard<std::mutex> lock(state);
    return timer && timer->armed;
}
esp_err_t esp_timer_get_expiry_time(esp_timer_handle_t timer, uint64_t *expiry) {
    if (!timer || !expiry) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state);
    if (!timer->armed || timer->period) return ESP_ERR_NOT_SUPPORTED;
    *expiry = uint64_t(timer->deadline);
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, uint32_t caps) {
    if (allocation_fails(size, caps, "heap_caps_malloc")) return nullptr;
    return remember(std::malloc(size ? size : 1), caps);
}
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    if (allocation_fails(n * size, caps, "heap_caps_calloc")) return nullptr;
    return remember(std::calloc(n ? n : 1, size ? size : 1), caps);
}
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    if (allocation_fails(size, caps, "heap_caps_realloc")) return nullptr;
    {
        std::lock_guard<std::mutex> lock(heap().mutex);
        heap().live.erase(ptr);
    }
    return remember(std::realloc(ptr, size ? size : 1), caps);
}
void heap_caps_free(void *ptr) {
    if (!ptr) return;
    {
        std::lock_guard<std::mutex> lock(heap().mutex);
        heap().live.erase(ptr);
    }
    std::free(ptr);
}
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback) {
    if (!callback) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(heap().mutex);
    heap().failed_hook = callback;
    return ESP_OK;
}
void heap_caps_malloc_extmem_enable(size_t limit) {
    std::lock_guard<std::mutex> lock(heap().mutex);
    heap().extmem_limit = limit;
}
size_t heap_caps_get_free_size(uint32_t caps) { return AMITY_SEAM(heap_caps_get_free_size, caps); }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return AMITY_SEAM(heap_caps_get_minimum_free_size, caps); }
size_t heap_caps_get_largest_free_block(uint32_t caps) { return AMITY_SEAM(heap_caps_get_largest_free_block, caps); }
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps) { AMITY_SEAM(heap_caps_get_info, info, caps); }

const esp_app_desc_t *esp_app_get_description(void) { return &description; }
int esp_app_get_elf_sha256(char *dst, size_t size) {
    if (!dst || size < 2) return 0;
    char text[65];
    for (std::size_t i = 0; i < 32; ++i) std::snprintf(text + 2 * i, 3, "%02x", description.app_elf_sha256[i]);
    const std::size_t count = std::min(size, sizeof(text));
    std::memcpy(dst, text, count);
    dst[count - 1] = 0;
    return int(count);
}

void vPortEnterCritical(portMUX_TYPE *mux) {
    critical.lock();
    ++mux->count;
}
void vPortExitCritical(portMUX_TYPE *mux) {
    if (!mux->count) fail("portEXIT_CRITICAL without portENTER_CRITICAL");
    --mux->count;
    critical.unlock();
}
BaseType_t xPortInIsrContext(void) { return isr.load() ? pdTRUE : pdFALSE; }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char *const pcName,
                                   const configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                                   UBaseType_t uxPriority, TaskHandle_t *const pvCreatedTask, const BaseType_t xCoreID) {
    return create_task(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask, xCoreID, 0, false);
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t pvTaskCode, const char *const pcName,
                                           const configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                                           UBaseType_t uxPriority, TaskHandle_t *const pvCreatedTask,
                                           const BaseType_t xCoreID, UBaseType_t uxMemoryCaps) {
    return create_task(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask, xCoreID, uxMemoryCaps,
                       true);
}
void vTaskDelete(TaskHandle_t xTaskToDelete) { delete_task(xTaskToDelete, false); }
void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete) { delete_task(xTaskToDelete, true); }
void vTaskDelay(const TickType_t xTicksToDelay) {
    std::function<void(TickType_t)> hook;
    {
        std::lock_guard<std::mutex> lock(state);
        ++delay_count;
        hook = delay_hook;
    }
    if (hook) hook(xTicksToDelay);
    std::unique_lock<std::mutex> lock(state);
    advance_to(lock, clock_us.load() + ticks_us(xTicksToDelay));
}
TickType_t xTaskGetTickCount(void) { return TickType_t(clock_us.load() / (int64_t(portTICK_PERIOD_MS) * 1000)); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return self()->task.handle; }
char *pcTaskGetName(TaskHandle_t xTaskToQuery) {
    tskTaskControlBlock *block = xTaskToQuery ? xTaskToQuery : self();
    return const_cast<char *>(block->task.name.c_str());
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask) {
    tskTaskControlBlock *block = xTask ? xTask : self();
    return block->task.stack_bytes / 2;
}
uint32_t ulTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait) {
    tskTaskControlBlock *block = self();
    std::unique_lock<std::mutex> lock(state);
    if (!wait(lock, xTicksToWait, "a task notification", [&] { return block->notifications > 0; })) return 0;
    const uint32_t value = block->notifications;
    block->notifications = xClearCountOnExit ? 0 : value - 1;
    return value;
}
BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify) {
    if (!xTaskToNotify) fail("xTaskNotifyGive to a null task");
    std::lock_guard<std::mutex> lock(state);
    ++xTaskToNotify->notifications;
    changed.notify_all();
    return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t xTaskToNotify, BaseType_t *pxHigherPriorityTaskWoken) {
    xTaskNotifyGive(xTaskToNotify);
    if (pxHigherPriorityTaskWoken) *pxHigherPriorityTaskWoken = pdFALSE;
}

QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize, const uint8_t) {
    if (!uxQueueLength) fail("xQueueCreate with no length");
    return create_queue(Queue::Kind::queue, uxQueueLength, uxItemSize, nullptr, nullptr, false);
}
QueueHandle_t xQueueGenericCreateStatic(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize,
                                        uint8_t *pucQueueStorage, StaticQueue_t *pxStaticQueue, const uint8_t) {
    if (!uxQueueLength || !pxStaticQueue) fail("xQueueCreateStatic without a length or control block");
    return create_queue(Queue::Kind::queue, uxQueueLength, uxItemSize, pucQueueStorage, pxStaticQueue, false);
}
QueueHandle_t xQueueCreateWithCaps(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, UBaseType_t) {
    if (!uxQueueLength) fail("xQueueCreateWithCaps with no length");
    return create_queue(Queue::Kind::queue, uxQueueLength, uxItemSize, nullptr, nullptr, true);
}
BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *const pvItemToQueue, TickType_t xTicksToWait,
                             const BaseType_t xCopyPosition) {
    std::unique_lock<std::mutex> lock(state);
    check_live(xQueue);
    if (is_mutex(xQueue)) fail("xQueueSend to a mutex");
    return wait(lock, xTicksToWait, "a full queue", [&] { return send_locked(xQueue, pvItemToQueue, xCopyPosition); })
               ? pdTRUE
               : errQUEUE_FULL;
}
BaseType_t xQueueGenericSendFromISR(QueueHandle_t xQueue, const void *const pvItemToQueue,
                                    BaseType_t *const pxHigherPriorityTaskWoken, const BaseType_t xCopyPosition) {
    if (pxHigherPriorityTaskWoken) *pxHigherPriorityTaskWoken = pdFALSE;
    std::lock_guard<std::mutex> lock(state);
    check_live(xQueue);
    return send_locked(xQueue, pvItemToQueue, xCopyPosition) ? pdTRUE : errQUEUE_FULL;
}
BaseType_t xQueueReceive(QueueHandle_t xQueue, void *const pvBuffer, TickType_t xTicksToWait) {
    std::unique_lock<std::mutex> lock(state);
    check_live(xQueue);
    if (is_mutex(xQueue)) fail("xQueueReceive from a mutex");
    return wait(lock, xTicksToWait, "an empty queue", [&] { return receive_locked(xQueue, pvBuffer, true); })
               ? pdTRUE
               : errQUEUE_EMPTY;
}
BaseType_t xQueuePeek(QueueHandle_t xQueue, void *const pvBuffer, TickType_t xTicksToWait) {
    std::unique_lock<std::mutex> lock(state);
    check_live(xQueue);
    return wait(lock, xTicksToWait, "an empty queue", [&] { return receive_locked(xQueue, pvBuffer, false); })
               ? pdTRUE
               : errQUEUE_EMPTY;
}
UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t xQueue) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xQueue);
    return xQueue->count;
}
UBaseType_t uxQueueSpacesAvailable(const QueueHandle_t xQueue) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xQueue);
    return xQueue->length - xQueue->count;
}
BaseType_t xQueueGenericReset(QueueHandle_t xQueue, BaseType_t) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xQueue);
    if (xQueue->kind != Queue::Kind::queue) fail("xQueueReset of a semaphore");
    xQueue->head = 0;
    xQueue->count = 0;
    changed.notify_all();
    return pdPASS;
}
static void delete_queue(QueueHandle_t xQueue, bool with_caps) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xQueue);
    auto &record = queue_records.at(xQueue->record);
    if (record.with_caps != with_caps) fail("a queue deleted with the wrong vQueueDelete variant");
    if (record.is_static && with_caps) fail("vQueueDeleteWithCaps of a static queue");
    record.deleted = true;
    if (xQueue->owns_storage) delete[] xQueue->storage;
    if (xQueue->is_static)
        xQueue->~QueueDefinition();
    else
        delete xQueue;
}
void vQueueDelete(QueueHandle_t xQueue) { delete_queue(xQueue, false); }
void vQueueDeleteWithCaps(QueueHandle_t xQueue) { delete_queue(xQueue, true); }

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    return create_queue(Queue::Kind::binary, 1, 0, nullptr, nullptr, false);
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *pxSemaphoreBuffer) {
    if (!pxSemaphoreBuffer) fail("xSemaphoreCreateBinaryStatic without a control block");
    return create_queue(Queue::Kind::binary, 1, 0, nullptr, pxSemaphoreBuffer, false);
}
static SemaphoreHandle_t create_mutex(Queue::Kind kind, StaticSemaphore_t *control) {
    QueueDefinition *mutex = create_queue(kind, 1, 0, nullptr, control, false);
    if (mutex) {
        std::lock_guard<std::mutex> lock(state);
        mutex->count = 1;
    }
    return mutex;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return create_mutex(Queue::Kind::mutex, nullptr); }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *pxMutexBuffer) {
    if (!pxMutexBuffer) fail("xSemaphoreCreateMutexStatic without a control block");
    return create_mutex(Queue::Kind::mutex, pxMutexBuffer);
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return create_mutex(Queue::Kind::recursive_mutex, nullptr); }
static SemaphoreHandle_t create_counting(UBaseType_t max, UBaseType_t initial, StaticSemaphore_t *control) {
    if (!max || initial > max) fail("xSemaphoreCreateCounting with an invalid count");
    QueueDefinition *semaphore = create_queue(Queue::Kind::counting, max, 0, nullptr, control, false);
    if (semaphore) {
        std::lock_guard<std::mutex> lock(state);
        semaphore->count = initial;
    }
    return semaphore;
}
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount) {
    return create_counting(uxMaxCount, uxInitialCount, nullptr);
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount,
                                                 StaticSemaphore_t *pxSemaphoreBuffer) {
    if (!pxSemaphoreBuffer) fail("xSemaphoreCreateCountingStatic without a control block");
    return create_counting(uxMaxCount, uxInitialCount, pxSemaphoreBuffer);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime) {
    std::unique_lock<std::mutex> lock(state);
    check_live(xSemaphore);
    if (xSemaphore->kind == Queue::Kind::queue) fail("xSemaphoreTake of a queue");
    if (xSemaphore->kind == Queue::Kind::recursive_mutex) fail("xSemaphoreTake of a recursive mutex");
    if (xSemaphore->kind == Queue::Kind::mutex && !xSemaphore->count && xSemaphore->holder == std::this_thread::get_id())
        fail("a thread took a mutex it already holds");
    return wait(lock, xBlockTime, "a semaphore", [&] { return receive_locked(xSemaphore, nullptr, true); }) ? pdTRUE
                                                                                                          : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xSemaphore);
    if (xSemaphore->kind == Queue::Kind::queue) fail("xSemaphoreGive of a queue");
    if (xSemaphore->kind == Queue::Kind::recursive_mutex) fail("xSemaphoreGive of a recursive mutex");
    return give_locked(xSemaphore);
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t xMutex, TickType_t xBlockTime) {
    std::unique_lock<std::mutex> lock(state);
    check_live(xMutex);
    if (xMutex->kind != Queue::Kind::recursive_mutex) fail("xSemaphoreTakeRecursive of a non-recursive semaphore");
    if (!xMutex->count && xMutex->holder == std::this_thread::get_id()) {
        ++xMutex->depth;
        return pdTRUE;
    }
    return wait(lock, xBlockTime, "a recursive mutex", [&] { return receive_locked(xMutex, nullptr, true); }) ? pdTRUE
                                                                                                             : pdFALSE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t xMutex) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xMutex);
    if (xMutex->kind != Queue::Kind::recursive_mutex) fail("xSemaphoreGiveRecursive of a non-recursive semaphore");
    if (xMutex->count || xMutex->holder != std::this_thread::get_id()) return pdFALSE;
    if (--xMutex->depth) return pdTRUE;
    return give_locked(xMutex);
}
BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken) {
    if (pxHigherPriorityTaskWoken) *pxHigherPriorityTaskWoken = pdFALSE;
    std::lock_guard<std::mutex> lock(state);
    check_live(xSemaphore);
    if (is_mutex(xSemaphore)) fail("xSemaphoreTakeFromISR of a mutex");
    return receive_locked(xSemaphore, nullptr, true) ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken) {
    if (pxHigherPriorityTaskWoken) *pxHigherPriorityTaskWoken = pdFALSE;
    std::lock_guard<std::mutex> lock(state);
    check_live(xSemaphore);
    if (is_mutex(xSemaphore)) fail("xSemaphoreGiveFromISR of a mutex");
    return send_locked(xSemaphore, nullptr, queueSEND_TO_BACK) ? pdTRUE : pdFALSE;
}
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t xSemaphore) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xSemaphore);
    return xSemaphore->count;
}
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t xSemaphore) {
    std::lock_guard<std::mutex> lock(state);
    check_live(xSemaphore);
    return is_mutex(xSemaphore) && !xSemaphore->count ? xSemaphore->holder_task : nullptr;
}

}  // extern "C"
