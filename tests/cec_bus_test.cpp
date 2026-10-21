/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime.hpp"
#include "cec/bus.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
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

static void capacity_and_overflow() {
    cec::Bus bus; assert(bus.start(1) == ESP_OK && bus.idle());
    for (unsigned i = 0; i < 8; ++i) assert(bus.send(broadcast));
    assert(!bus.idle() && !bus.send(broadcast));
    assert(!bus.send(release)); // Opcode alone never grants overflow admission.
    assert(bus.send_guarded(broadcast, {}, 1000, 1, true)); // Flag is opcode agnostic.
    const auto denied_at = now;
    cec::TxResult result = cec::TxResult::Success;
    assert(!bus.send_guarded(release, {}, 1000, 3, true));
    assert(!bus.send_and_wait_guarded(release, {}, 1000, 3, &result, true));
    assert(result == cec::TxResult::Pending && now == denied_at); // Full ninth drops, never waits.
    assert(!bus.send(broadcast));
    step();
    assert(bus.transmit_guarded(broadcast, {nullptr, 0, nullptr, 0, now}) == cec::TxResult::Expired);
    pump();
    assert(bus.idle() && bus.statistics().transmitted == 9 && starts[1] == 9);
    pump(); assert(starts[1] == 9); // A dropped release has no hidden retry/backlog.
    // An explicit ordinary CEC admission wait retains the consort API policy.
    for (unsigned i = 0; i < 8; ++i) assert(bus.send(broadcast));
    assert(bus.send(broadcast, 1000)); pump();
    assert(bus.statistics().transmitted == 18 && bus.idle());
    std::puts("PASS FIFO eight normal / ninth flagged / later immediate drop / explicit bounded wait");
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
        assert(bus.send_guarded(broadcast, {}, 0, 1, true));
        assert(!bus.send_guarded(release, {}, 1000, 3, true));
    };
    pump(); before_tick = {};
    assert(inspected && bus.idle() && bus.statistics().transmitted == 9);
    std::puts("PASS queued+active shared capacity / no popped-work idle or overflow bypass");
}
static void all_public_paths_and_attempts() {
    cec::Bus bus; assert(bus.start(3) == ESP_OK);
    auto attempts = [&](unsigned expected, auto action) {
        const auto prior = starts[3]; action(); pump(); assert(starts[3] - prior == expected);
    };
    attempts(3, [&] { assert(bus.send(press)); });
    attempts(1, [&] { assert(bus.send_guarded(press, {}, 0, 1)); });
    attempts(3, [&] { assert(bus.send_guarded(press, {}, 0, 3)); }); // No opcode attempt policy.
    attempts(3, [&] { assert(bus.send(release)); });
    attempts(1, [&] { assert(bus.send_and_wait(press)); });
    cec::TxResult result = cec::TxResult::Pending;
    attempts(1, [&] { assert(bus.send_and_wait_guarded(press, {}, 0, 1, &result)); });
    assert(result == cec::TxResult::Nack); // Legacy bool means admitted/completed, not ACK.
    attempts(1, [&] { assert(bus.send_and_wait_guarded(broadcast, {}, 0, 1, &result)); });
    assert(result == cec::TxResult::Success);
    attempts(3, [&] { assert(bus.transmit(press, 3) == cec::TxResult::Nack); });
    attempts(2, [&] { assert(bus.transmit_guarded(press, {}, 2) == cec::TxResult::Nack); });
    const auto transmitted = bus.statistics().transmitted;
    assert(bus.send(broadcast));
    assert(bus.transmit(press, 1) == cec::TxResult::Nack);
    assert(bus.statistics().transmitted == transmitted + 1); // transmit waited behind the FIFO head.
    const auto prior = starts[3];
    assert(!bus.send_guarded(press, {}, 0, 0) && !bus.send_and_wait(press, 0, 0));
    assert(bus.transmit(press, 0) == cec::TxResult::LineError);
    const cec::Frame empty{};
    assert(!bus.send(empty) && !bus.send_and_wait(empty) && bus.transmit(empty) == cec::TxResult::LineError);
    pump(); assert(starts[3] == prior);
    std::puts("PASS all public send paths FIFO / caller attempt budgets / terminal result / invalid admission");
}
static void guard_boundaries() {
    cec::Bus bus; assert(bus.start(4) == ESP_OK);
    std::atomic<uint32_t> generation{1}, secondary{1};
    cec::ExecutionFence fence{&generation, 1, &secondary, 1};
    assert(bus.send_guarded(press, fence, 0, 1));
    generation = 2; pump(); assert(starts[4] == 0 && bus.idle());
    assert(!bus.send_guarded(press, fence) && !bus.send_and_wait_guarded(press, fence));
    assert(bus.transmit_guarded(press, fence) == cec::TxResult::Cancelled);
    generation = 1;
    external_pointer = &generation;
    assert(!bus.send_guarded(press, fence));
    assert(bus.transmit_guarded(press, fence) == cec::TxResult::LineError);
    external_pointer = nullptr;
    cec::ExecutionFence misaligned{reinterpret_cast<const std::atomic<uint32_t>*>(reinterpret_cast<uintptr_t>(&generation) + 1), 1};
    assert(!bus.send_guarded(press, misaligned));
    // A queued guard expires without generating an edge, including transmit().
    fence.expires_at_us = now + 1;
    assert(bus.send_guarded(press, fence)); step(); pump(); assert(starts[4] == 0);
    assert(bus.transmit_guarded(press, fence) == cec::TxResult::Expired);
    fence.expires_at_us = 0;
    external_low = uint64_t{1} << 4;
    unsigned ticks = 0;
    before_tick = [&] { if (++ticks == 5) secondary = 2; };
    cec::TxResult result = cec::TxResult::Pending;
    assert(!bus.send_and_wait_guarded(press, fence, 0, 3, &result));
    before_tick = {}; external_low = 0;
    assert(result == cec::TxResult::Cancelled && starts[4] == 0);
    secondary = 1;
    // After the actual first edge a frame is allowed to finish; revoked authority
    // prevents a later attempt instead of truncating the electrical waveform.
    const auto prior = starts[4];
    before_tick = [&] { if (starts[4] > prior) generation = 2; };
    assert(!bus.send_and_wait_guarded(press, fence, 0, 3, &result));
    before_tick = {};
    assert(result == cec::TxResult::Cancelled && starts[4] == prior + 1);
    generation = 1;
    const auto success_prior = starts[4];
    before_tick = [&] { if (starts[4] > success_prior) generation = 2; };
    assert(bus.send_and_wait_guarded(broadcast, fence, 0, 3, &result));
    before_tick = {};
    assert(result == cec::TxResult::Success && starts[4] == success_prior + 1);
    std::puts("PASS queued/first-edge/retry guard and expiry / internal aligned counters / in-flight waveform finishes");
}
static void independent_buses() {
    cec::Bus a, b; assert(a.start(5) == ESP_OK && b.start(6) == ESP_OK);
    for (unsigned i = 0; i < 8; ++i) { assert(a.send(broadcast)); assert(b.send(broadcast)); }
    assert(!a.send(broadcast) && !b.send(broadcast));
    assert(a.send_guarded(release, {}, 0, 1, true));
    assert(b.send_guarded(release, {}, 0, 1, true));
    pump(); assert(a.idle() && b.idle());
    assert(a.statistics().transmitted == 8 && b.statistics().transmitted == 8);
    std::puts("PASS each wire owns its FIFO and completion accounting");
}
int main() {
    capacity_and_overflow(); active_slot_is_reserved(); all_public_paths_and_attempts();
    guard_boundaries(); independent_buses();
    assert(tasks.empty() && timers.empty() && output_low == 0);
    std::puts("CEC actual Bus/Engine driver tests PASS (ASan/UBSan)");
}
