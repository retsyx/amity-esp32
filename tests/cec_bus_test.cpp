/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime.hpp"
#include "cec/bus.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// The production Bus worker and ISR run against a deterministic 50-us clock.
// These fakes model only RTOS/GPIO scheduling, not CEC message decisions.
struct Queue { size_t capacity, item_size; std::deque<std::vector<uint8_t>> items; };
struct Semaphore { bool available = false; };
struct Task { void (*code)(void *); void *arg; bool pumping = false; };
struct FakeTimer { TimerCallback callback = nullptr; void *arg = nullptr; bool running = false; };
struct EmptyQueue {};
Gpio GPIO;
const void *external_pointer = nullptr;
static uint64_t now = 0, output_low = 0, external_low = 0;
static std::array<uint64_t, 64> released_at{};
static std::array<unsigned, 64> starts{}, low_edges{};
static std::vector<Task *> tasks;
static std::vector<FakeTimer *> timers;
static std::function<void()> before_tick;
static std::vector<std::string> drop_logs;
void test_cec_log(const char *tag, const char *format, ...) {
    assert(std::strcmp(tag, "cec") == 0);
    char message[256];
    va_list args; va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    drop_logs.emplace_back(message);
}
static void step() {
    now += 50;
    if (before_tick) before_tick();
    for (auto *timer : timers) if (timer->running) timer->callback(timer, nullptr, timer->arg);
}
static void pump() {
    for (auto *task : tasks) if (!task->pumping) {
        task->pumping = true;
        try { task->code(task->arg); } catch (const EmptyQueue &) {}
        task->pumping = false;
    }
}
QueueHandle_t xQueueCreate(unsigned count, size_t size) { return new Queue{count, size, {}}; }
int xQueueSend(QueueHandle_t h, const void *data, uint32_t) {
    auto &q = *static_cast<Queue *>(h);
    if (q.items.size() >= q.capacity) return pdFALSE;
    const auto *bytes = static_cast<const uint8_t *>(data);
    q.items.push_back({bytes, bytes + q.item_size}); return pdTRUE;
}
int xQueueSendFromISR(QueueHandle_t q, const void *data, BaseType_t *) { return xQueueSend(q, data, 0); }
int xQueueReceive(QueueHandle_t h, void *out, uint32_t timeout) {
    auto &q = *static_cast<Queue *>(h);
    if (q.items.empty() && timeout == portMAX_DELAY) throw EmptyQueue{};
    const auto deadline = now + uint64_t{timeout} * 1000;
    while (q.items.empty() && now < deadline) step();
    if (q.items.empty()) return pdFALSE;
    std::memcpy(out, q.items.front().data(), q.item_size); q.items.pop_front(); return pdTRUE;
}
void xQueueReset(QueueHandle_t h) { static_cast<Queue *>(h)->items.clear(); }
unsigned uxQueueMessagesWaiting(QueueHandle_t h) { return static_cast<Queue *>(h)->items.size(); }
void vQueueDelete(QueueHandle_t h) { delete static_cast<Queue *>(h); }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new Semaphore{true}; }
SemaphoreHandle_t xSemaphoreCreateBinary() { return new Semaphore; }
int xSemaphoreTake(SemaphoreHandle_t h, uint32_t timeout) {
    auto &s = *static_cast<Semaphore *>(h);
    const auto deadline = now + uint64_t{timeout} * 1000;
    unsigned passes = 0;
    while (!s.available && timeout && passes++ < 100000) {
        pump();
        if (s.available) break;
        if (timeout != portMAX_DELAY && now >= deadline) break;
        step();
    }
    if (!s.available) return pdFALSE;
    s.available = false; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t h) { static_cast<Semaphore *>(h)->available = true; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t h) { delete static_cast<Semaphore *>(h); }
int xTaskCreate(void (*code)(void *), const char *, unsigned bytes, void *arg, unsigned, TaskHandle_t *out) {
    assert(bytes == 3072);
    auto *task = new Task{code, arg}; tasks.push_back(task); *out = task; return pdPASS;
}
void vTaskDelete(TaskHandle_t h) {
    auto *task = static_cast<Task *>(h);
    tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end()); delete task;
}
int64_t esp_timer_get_time() { return now; }
esp_err_t gpio_config(const gpio_config_t *) { return ESP_OK; }
int gpio_ll_get_level(Gpio *, gpio_num_t pin) { return !((output_low | external_low) & (uint64_t{1} << pin)); }
void gpio_ll_set_level(Gpio *, gpio_num_t pin, int high) {
    const uint64_t mask = uint64_t{1} << pin;
    if (!high) {
        if (!(output_low & mask)) {
            ++low_edges[pin];
            if (now - released_at[pin] >= 7000) ++starts[pin];
        }
        output_low |= mask;
    } else {
        if (output_low & mask) released_at[pin] = now;
        output_low &= ~mask;
    }
}
esp_err_t gpio_set_level(gpio_num_t pin, int high) { gpio_ll_set_level(&GPIO, pin, high); return ESP_OK; }
esp_err_t gptimer_new_timer(const gptimer_config_t *config, gptimer_handle_t *out) {
    assert(config->resolution_hz == 1000000 && config->intr_priority == 3 && config->flags.intr_shared);
    auto *timer = new FakeTimer; timers.push_back(timer); *out = timer; return ESP_OK;
}
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t h, const gptimer_event_callbacks_t *c, void *arg) {
    h->callback = c->on_alarm; h->arg = arg; return ESP_OK;
}
esp_err_t gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t *a) { assert(a->alarm_count == 50); return ESP_OK; }
esp_err_t gptimer_enable(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_disable(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_start(gptimer_handle_t h) { h->running = true; return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t h) { h->running = false; return ESP_OK; }
esp_err_t gptimer_del_timer(gptimer_handle_t h) {
    timers.erase(std::remove(timers.begin(), timers.end(), h), timers.end()); delete h; return ESP_OK;
}

static const cec::Frame broadcast{{0x4f, 0x85}, 2};
static const cec::Frame press{{0x40, 0x44, 0x41}, 3};
static const cec::Frame release{{0x40, 0x45}, 2};

static void capacity_and_drop_logging() {
    cec::Bus bus; assert(bus.start(1) == ESP_OK && bus.idle());
    const auto prior_logs = drop_logs.size();
    for (unsigned i = 0; i < 8; ++i) assert(bus.send(broadcast));
    assert(drop_logs.size() == prior_logs);
    assert(!bus.idle() && !bus.send(broadcast));
    assert(drop_logs.back() == "TX dropped: FIFO full (GPIO 1, 2 bytes: 4f 85)");
    assert(!bus.send(release));
    assert(drop_logs.back() == "TX dropped: FIFO full (GPIO 1, 2 bytes: 40 45)");
    const auto denied_at = now;
    cec::TxCompletion completion; completion.result.store(cec::TxResult::Success);
    assert(!bus.send_guarded(release, {}, 3));
    assert(!bus.send_async_guarded(release, {}, completion, 3));
    assert(completion.result.load() == cec::TxResult::Success && now == denied_at);
    assert(!bus.send_guarded(press, {}, 1));
    assert(drop_logs.back() == "TX dropped: FIFO full (GPIO 1, 3 bytes: 40 44 41)");
    cec::Frame longest; longest.size = 16;
    for (unsigned i = 0; i < longest.size; ++i) longest.data[i] = i;
    assert(!bus.send(longest));
    assert(drop_logs.back() == "TX dropped: FIFO full (GPIO 1, 16 bytes: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f)");
    const cec::Frame poll{{0x44}, 1};
    assert(!bus.send(poll));
    assert(drop_logs.back() == "TX dropped: FIFO full (GPIO 1, 1 bytes: 44)");
    assert(drop_logs.size() == prior_logs + 7 && now == denied_at);
    pump();
    assert(bus.idle() && bus.statistics().transmitted == 8 && starts[1] == 8);
    pump(); assert(starts[1] == 8); // A dropped release has no hidden retry/backlog.
    for (unsigned i = 0; i < 8; ++i) assert(bus.send(broadcast));
    const auto full_at = now;
    assert(!bus.send(broadcast) && !bus.send_guarded(broadcast, {}));
    assert(now == full_at); // No public admission path waits for capacity.
    assert(drop_logs.size() == prior_logs + 9);
    pump(); assert(bus.statistics().transmitted == 16 && bus.idle());
    assert(drop_logs.size() == prior_logs + 9);
    std::puts("PASS FIFO eight including releases / every full admission drops immediately and logs its wire/frame");
}
static void active_slot_is_reserved() {
    cec::Bus bus; assert(bus.start(2) == ESP_OK);
    assert(bus.send(broadcast));
    bool inspected = false;
    before_tick = [&] {
        if (inspected) return;
        inspected = true; assert(!bus.idle());
        for (unsigned i = 0; i < 7; ++i) assert(bus.send(broadcast));
        assert(!bus.send(broadcast));
        assert(!bus.send_guarded(release, {}, 3));
    };
    pump(); before_tick = {};
    assert(inspected && bus.idle() && bus.statistics().transmitted == 8);
    std::puts("PASS queued+active shared capacity / no popped-work idle or release bypass");
}
static void all_public_paths_and_attempts() {
    cec::Bus bus; assert(bus.start(3) == ESP_OK);
    auto attempts = [&](unsigned expected, auto action) {
        const auto prior = starts[3]; action(); pump(); assert(starts[3] - prior == expected);
    };
    attempts(3, [&] { assert(bus.send(press)); });
    attempts(1, [&] { assert(bus.send_guarded(press, {}, 1)); });
    attempts(3, [&] { assert(bus.send_guarded(press, {}, 3)); }); // No opcode attempt policy.
    attempts(3, [&] { assert(bus.send(release)); });
    cec::TxCompletion completion;
    attempts(1, [&] { assert(bus.send_async_guarded(press, {}, completion)); });
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    attempts(1, [&] { assert(bus.send_async_guarded(broadcast, {}, completion)); });
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Success);
    attempts(3, [&] { assert(bus.send_async_guarded(press, {}, completion, 3)); });
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    attempts(2, [&] { assert(bus.send_async_guarded(press, {}, completion, 2)); });
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    const auto transmitted = bus.statistics().transmitted;
    assert(bus.send(broadcast));
    assert(bus.send_async_guarded(press, {}, completion)); pump();
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    assert(bus.statistics().transmitted == transmitted + 1); // Completion follows the FIFO head.
    const auto prior = starts[3];
    assert(!bus.send_guarded(press, {}, 0) && !bus.send_async_guarded(press, {}, completion, 0));
    const cec::Frame empty{};
    assert(!bus.send(empty) && !bus.send_async_guarded(empty, {}, completion));
    assert(completion.result.load() == cec::TxResult::Nack);
    pump(); assert(starts[3] == prior);
    std::puts("PASS all public admission paths FIFO / caller attempt budgets / terminal result / invalid admission");
}
static void guard_boundaries() {
    cec::Bus bus; assert(bus.start(4) == ESP_OK);
    std::atomic<uint32_t> generation{1}, secondary{1};
    cec::ExecutionFence fence{&generation, 1, &secondary, 1};
    cec::TxCompletion completion;
    assert(bus.send_async_guarded(press, fence, completion));
    generation = 2; pump(); assert(starts[4] == 0 && bus.idle());
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Cancelled);
    assert(!bus.send_guarded(press, fence) && !bus.send_async_guarded(press, fence, completion));
    assert(completion.result.load() == cec::TxResult::Cancelled);
    generation = 1;
    external_pointer = &generation;
    assert(!bus.send_guarded(press, fence) && !bus.send_async_guarded(press, fence, completion));
    external_pointer = nullptr;
    cec::ExecutionFence misaligned{reinterpret_cast<const std::atomic<uint32_t>*>(reinterpret_cast<uintptr_t>(&generation) + 1), 1};
    assert(!bus.send_guarded(press, misaligned) && !bus.send_async_guarded(press, misaligned, completion));
    // A queued guard expires without generating an edge.
    fence.expires_at_us = now + 1;
    assert(bus.send_async_guarded(press, fence, completion)); step(); pump();
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Expired && starts[4] == 0);
    fence.expires_at_us = 0;
    external_low = uint64_t{1} << 4;
    unsigned ticks = 0;
    before_tick = [&] { if (++ticks == 5) secondary = 2; };
    assert(bus.send_async_guarded(press, fence, completion, 3)); pump();
    before_tick = {}; external_low = 0;
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Cancelled && starts[4] == 0);
    secondary = 1;
    // After the actual first edge a frame is allowed to finish; revoked authority
    // prevents a later attempt instead of truncating the electrical waveform.
    const auto prior = starts[4];
    before_tick = [&] { if (starts[4] > prior) generation = 2; };
    assert(bus.send_async_guarded(press, fence, completion, 3)); pump();
    before_tick = {};
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Cancelled && starts[4] == prior + 1);
    generation = 1;
    const auto success_prior = starts[4];
    before_tick = [&] { if (starts[4] > success_prior) generation = 2; };
    assert(bus.send_async_guarded(broadcast, fence, completion, 3)); pump();
    before_tick = {};
    assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Success && starts[4] == success_prior + 1);
    std::puts("PASS queued/first-edge/retry guard and expiry / internal aligned counters / in-flight waveform finishes");
}
static void independent_buses() {
    cec::Bus a, b; assert(a.start(5) == ESP_OK && b.start(6) == ESP_OK);
    for (unsigned i = 0; i < 8; ++i) { assert(a.send(broadcast)); assert(b.send(broadcast)); }
    assert(!a.send(broadcast) && !b.send(broadcast));
    const auto prior_logs = drop_logs.size();
    assert(!a.send_guarded(release, {}, 1));
    assert(!b.send_guarded(release, {}, 1));
    assert(drop_logs.size() == prior_logs + 2);
    assert(drop_logs[prior_logs] == "TX dropped: FIFO full (GPIO 5, 2 bytes: 40 45)");
    assert(drop_logs[prior_logs + 1] == "TX dropped: FIFO full (GPIO 6, 2 bytes: 40 45)");
    pump(); assert(a.idle() && b.idle());
    assert(a.statistics().transmitted == 8 && b.statistics().transmitted == 8);
    std::puts("PASS each wire owns its FIFO and completion accounting");
}
static void asynchronous_capacity_and_completion() {
    cec::Bus bus; assert(bus.start(7) == ESP_OK);
    std::array<cec::TxCompletion, 8> completed;
    const auto admitted_at = now;
    const auto prior_starts = starts[7];
    for (auto &completion : completed) {
        completion.result.store(cec::TxResult::Success);
        assert(bus.send_async_guarded(broadcast, {}, completion));
        assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Pending);
    }
    assert(now == admitted_at && starts[7] == prior_starts && !bus.idle());
    cec::TxCompletion rejected;
    rejected.result.store(cec::TxResult::Nack);
    assert(!bus.send_async_guarded(broadcast, {}, rejected));
    assert(rejected.result.load() == cec::TxResult::Nack && now == admitted_at);
    bool refilled = false;
    before_tick = [&] {
        if (refilled || completed[0].result.load(std::memory_order_acquire) == cec::TxResult::Pending) return;
        refilled = true;
        assert(completed[0].result.load() == cec::TxResult::Success);
        assert(completed[1].result.load() == cec::TxResult::Pending);
        // Seeing completion guarantees that the first operation's capacity has
        // already been returned, even while the following frame is on the wire.
        const auto queued_at = now;
        assert(bus.send_async_guarded(broadcast, {}, rejected));
        assert(rejected.result.load() == cec::TxResult::Pending && now == queued_at);
    };
    pump(); before_tick = {};
    assert(refilled && bus.idle() && starts[7] == prior_starts + 9);
    for (auto &completion : completed) assert(completion.result.load(std::memory_order_acquire) == cec::TxResult::Success);
    assert(rejected.result.load(std::memory_order_acquire) == cec::TxResult::Success);
    // A completed tracker is reusable, and failed admission does not reset it.
    assert(!bus.send_async_guarded({}, {}, rejected));
    assert(!bus.send_async_guarded(press, {}, rejected, 0));
    assert(rejected.result.load() == cec::TxResult::Success);
    assert(bus.send_async_guarded(press, {}, rejected)); pump();
    assert(rejected.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    std::puts("PASS async zero-wait admission / individual completion / capacity released before publication / tracker reuse");
}
static void asynchronous_terminal_outcomes() {
    cec::Bus bus; assert(bus.start(8) == ESP_OK);
    cec::TxCompletion acknowledged, failed, cancelled, expired;
    std::atomic<uint32_t> generation{1};
    assert(bus.send_async_guarded(broadcast, {}, acknowledged));
    assert(bus.send_async_guarded(press, {}, failed, 2));
    assert(bus.send_async_guarded(press, {&generation, 1}, cancelled));
    assert(bus.send_async_guarded(press, {nullptr, 0, nullptr, 0, now + 1}, expired));
    generation = 2; step(); pump();
    assert(acknowledged.result.load(std::memory_order_acquire) == cec::TxResult::Success);
    assert(failed.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    assert(cancelled.result.load(std::memory_order_acquire) == cec::TxResult::Cancelled);
    assert(expired.result.load(std::memory_order_acquire) == cec::TxResult::Expired);
    assert(starts[8] == 3 && bus.idle());
    assert(bus.statistics().transmitted == 1 && bus.statistics().tx_errors == 1);
    const auto rejected_at = now;
    assert(!bus.send_async_guarded(press, {&generation, 1}, acknowledged));
    assert(acknowledged.result.load() == cec::TxResult::Success && now == rejected_at);
    std::puts("PASS async success/failure/cancellation/expiry remain independent / caller attempt budget");
}
static void asynchronous_admission_during_transmission() {
    cec::Bus a, b; assert(a.start(9) == ESP_OK && b.start(10) == ESP_OK);
    cec::TxCompletion active, same_wire, other_wire;
    assert(a.send_async_guarded(press, {}, active));
    bool admitted = false;
    before_tick = [&] {
        if (admitted || !starts[9]) return;
        admitted = true;
        assert(active.result.load(std::memory_order_acquire) == cec::TxResult::Pending);
        const auto queued_at = now;
        assert(a.send_async_guarded(broadcast, {}, same_wire));
        assert(b.send_async_guarded(broadcast, {}, other_wire));
        assert(now == queued_at);
        assert(same_wire.result.load() == cec::TxResult::Pending);
        assert(other_wire.result.load() == cec::TxResult::Pending);
    };
    pump(); before_tick = {};
    assert(admitted && a.idle() && b.idle());
    assert(active.result.load(std::memory_order_acquire) == cec::TxResult::Nack);
    assert(same_wire.result.load(std::memory_order_acquire) == cec::TxResult::Success);
    assert(other_wire.result.load(std::memory_order_acquire) == cec::TxResult::Success);
    assert(starts[9] == 2 && starts[10] == 1);
    std::puts("PASS async admission during outstanding ACK / same-wire FIFO / independent bus completions");
}
int main() {
    capacity_and_drop_logging(); active_slot_is_reserved(); all_public_paths_and_attempts();
    guard_boundaries(); independent_buses(); asynchronous_capacity_and_completion();
    asynchronous_terminal_outcomes(); asynchronous_admission_during_transmission();
    assert(tasks.empty() && timers.empty() && output_low == 0);
    std::puts("CEC actual Bus/Engine driver tests PASS (ASan/UBSan)");
}
