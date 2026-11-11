/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime.hpp"
#include "cec/bus.hpp"
#include "amity/hardware_limits.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include <array>
#include <cstdarg>
#include <string>

struct Object { virtual ~Object() = default; };
struct Queue : Object { size_t capacity, item_size; std::deque<std::vector<uint8_t>> items; };
struct Semaphore : Object { unsigned available = 0; bool mutex = false; };
static std::vector<Semaphore*> semaphores;
static std::vector<std::string> warning_logs;
void fixture_log_warning(const char* tag, const char* format, ...) {
    assert(std::strcmp(tag, "cec") == 0 && critical_depth == 0);
    char message[384]; va_list args; va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args); va_end(args);
    unsigned locked_mutexes = 0;
    for (const auto* semaphore : semaphores)
        if (semaphore->mutex && !semaphore->available) ++locked_mutexes;
    // Worker diagnostics retain the lifecycle lock protecting port lifetime.
    // Admission/drop logging must still run with every mutex released.
    const bool worker_diagnostic = std::strncmp(message, "TX failed (", 11) == 0;
    assert(locked_mutexes == (worker_diagnostic ? 1u : 0u));
    warning_logs.emplace_back(message);
}
struct Task { void (*code)(void *); void *arg; bool pumping = false; };
struct FakeTimer { TimerCallback callback = nullptr; void *arg = nullptr; bool running = false; };
struct EmptyQueue {};
Gpio GPIO;
static uint64_t now = 0, output_low = 0, external_low = 0;
static unsigned timers_created = 0, low_edges = 0;
static std::array<unsigned, 64> pin_edges{};
static bool fail_task_creation = false;
static std::vector<Task *> tasks;
static FakeTimer *timer = nullptr;
static std::function<void()> before_tick;
static void step() {
    now += 50;
    if (before_tick) before_tick();
    if (timer && timer->running) timer->callback(timer, nullptr, timer->arg);
}
static void pump() {
    for (auto *task : tasks) if (!task->pumping) {
        task->pumping = true;
        try { task->code(task->arg); } catch (const EmptyQueue &) {}
        task->pumping = false;
    }
}
QueueHandle_t xQueueCreateStatic(unsigned count, size_t size, uint8_t *, StaticQueue_t *) {
    auto *q = new Queue; q->capacity = count; q->item_size = size; return q;
}
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
void vQueueDelete(QueueHandle_t h) { delete static_cast<Object *>(h); }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *) { auto *s = new Semaphore; s->available = true; s->mutex = true; semaphores.push_back(s); return s; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *) { return new Semaphore; }
SemaphoreHandle_t xSemaphoreCreateCountingStatic(unsigned, unsigned initial, StaticSemaphore_t *) { auto *s = new Semaphore; s->available = initial; return s; }
int xSemaphoreTake(SemaphoreHandle_t h, uint32_t timeout) {
    auto &s = *static_cast<Semaphore *>(h);
    unsigned passes = 0;
    while (!s.available && timeout && passes++ < 100000) {
        const auto before = now; pump();
        if (timeout != portMAX_DELAY || before == now) break;
    }
    if (!s.available) return pdFALSE;
    --s.available; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t h) { ++static_cast<Semaphore *>(h)->available; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t h) {
    auto* s = static_cast<Semaphore*>(h);
    semaphores.erase(std::remove(semaphores.begin(), semaphores.end(), s), semaphores.end()); delete s;
}
int xTaskCreateWithCaps(void (*code)(void *), const char *, unsigned bytes, void *arg, unsigned,
                        TaskHandle_t *out, unsigned caps) {
    assert(bytes == 3072 && (caps & MALLOC_CAP_INTERNAL));
    if (fail_task_creation) return pdFALSE;
    auto *task = new Task{code, arg}; tasks.push_back(task); *out = task; return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t h) {
    auto *task = static_cast<Task *>(h);
    tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end()); delete task;
}
void vTaskDelay(uint32_t ticks) { for (uint32_t i = 0; i < ticks * 20; ++i) step(); throw EmptyQueue{}; }
int64_t esp_timer_get_time() { return now; }
esp_err_t gpio_config(const gpio_config_t *) { return ESP_OK; }
void gpio_ll_set_level(Gpio *, gpio_num_t pin, int high) {
    const uint64_t mask = uint64_t{1} << pin;
    if (!high) { if (!(output_low & mask)) { ++low_edges; ++pin_edges[pin]; } output_low |= mask; }
    else output_low &= ~mask;
    GPIO.in = static_cast<uint32_t>(~(output_low | external_low)); GPIO.in1.val = static_cast<uint32_t>(~(output_low | external_low) >> 32);
}
esp_err_t gpio_set_level(gpio_num_t pin, int high) { gpio_ll_set_level(&GPIO, pin, high); return ESP_OK; }
esp_err_t gptimer_new_timer(const gptimer_config_t *config, gptimer_handle_t *out) {
    assert(!timer && config->resolution_hz == 1000000); ++timers_created;
    timer = new FakeTimer; *out = timer; return ESP_OK;
}
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t h, const gptimer_event_callbacks_t *c, void *arg) {
    h->callback = c->on_alarm; h->arg = arg; return ESP_OK;
}
esp_err_t gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t *a) { assert(a->alarm_count == 50); return ESP_OK; }
esp_err_t gptimer_enable(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_disable(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_start(gptimer_handle_t h) { h->running = true; return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t h) { h->running = false; return ESP_OK; }
esp_err_t gptimer_del_timer(gptimer_handle_t h) { assert(h == timer); delete timer; timer = nullptr; return ESP_OK; }

static void finish(cec::TxCompletion& completion) {
    for (unsigned pass=0;pass<4000&&completion.result.load(std::memory_order_acquire)==cec::TxResult::Pending;++pass) pump();
    assert(completion.result.load(std::memory_order_acquire)!=cec::TxResult::Pending);
}

static void caller_attempt_tests() {
    const cec::Frame press{{0x40, 0x44, 0x41}, 3}, power{{0x40, 0x44, 0x6d}, 3};
    const cec::Frame release{{0x40, 0x45}, 2}, query{{0x40, 0x8f}, 2}, poll{{0x44}, 1};
    std::atomic<uint32_t> generation{1};const cec::ExecutionFence fence{&generation,1};
    {
        cec::Bus bus;assert(bus.start(30)==ESP_OK);
        auto expect_attempts=[&](unsigned attempts,auto action){
            const auto before=bus.statistics();const auto edges=pin_edges[30];
            action();for(unsigned pass=0;pass<4000;++pass)pump();
            const auto after=bus.statistics();
            assert(after.retries-before.retries==attempts-1&&pin_edges[30]>edges);
        };
        // No receiver ACKs this line. The real worker/Engine must stop after
        // exactly the caller-provided budget; no opcode policy belongs here.
        expect_attempts(3,[&]{assert(bus.send(press));});
        expect_attempts(3,[&]{assert(bus.send_guarded(press,fence,3));});
        cec::TxCompletion outcome;
        expect_attempts(1,[&]{
            const auto admitted_at=now;
            assert(bus.send_async_guarded(press,fence,outcome,1));
            assert(now==admitted_at&&outcome.result.load()==cec::TxResult::Pending);
        });
        assert(outcome.result.load(std::memory_order_acquire)==cec::TxResult::Nack);
        expect_attempts(3,[&]{assert(bus.send_async_guarded(press,fence,outcome,3));});
        assert(outcome.result.load(std::memory_order_acquire)==cec::TxResult::Nack);
        expect_attempts(8,[&]{assert(bus.send_guarded(power,fence,8));});
        expect_attempts(3,[&]{assert(bus.send(release));});
        expect_attempts(3,[&]{assert(bus.send_guarded(query,fence,3));});
        expect_attempts(3,[&]{assert(bus.send_guarded(poll,{},3));}); // Header0x44 alone is a poll, not an opcode.
        const auto edges=pin_edges[30];
        assert(!bus.send_guarded(press,fence,0)&&!bus.send_async_guarded(press,fence,outcome,0));
        pump();assert(pin_edges[30]==edges); // Invalid zero budget never becomes a send.
        const auto errors=bus.statistics().tx_errors;
        for(unsigned queued=0;queued<8;++queued)assert(bus.send(press));
        assert(!bus.send(press)); // Legacy and native TX FIFOs share the explicit eight-message bound.
        for(unsigned pass=0;pass<4000;++pass)pump();assert(bus.statistics().tx_errors==errors+8&&bus.statistics().tx_queue_high_water==8);
    }
    {
        cec::Bus bus;assert(bus.start(31,cec::Bus::QueueProfile::Native)==ESP_OK);
        auto expect_attempts=[&](unsigned attempts,auto action){
            const auto before=bus.statistics();const auto edges=pin_edges[31];action();
            for(unsigned pass=0;pass<500;++pass)pump();
            assert(bus.statistics().retries-before.retries==attempts-1&&pin_edges[31]>edges);
        };
        expect_attempts(3,[&]{assert(bus.send(press));});
        expect_attempts(3,[&]{assert(bus.send_guarded(power,fence,3));});
        cec::TxCompletion outcome;
        for (const auto& frame : {press, release, query, poll}) {
            expect_attempts(3,[&]{assert(bus.send_async_guarded(frame,fence,outcome,3));});
            assert(outcome.result.load(std::memory_order_acquire)==cec::TxResult::Nack);
        }
    }
    assert(!timer&&tasks.empty()&&!output_low);
}

static void terminal_failure_diagnostics_tests() {
    const cec::Frame press{{0x40, 0x44, 0x6d}, 3}, broadcast{{0x4f, 0x36}, 2}, poll{{0x44}, 1};
    constexpr unsigned pin = 36;
    constexpr uint64_t mask = uint64_t{1} << pin;
    for (auto profile : {cec::Bus::QueueProfile::Legacy, cec::Bus::QueueProfile::Native}) {
        cec::Bus bus; assert(bus.start(pin, profile) == ESP_OK);
        cec::TxCompletion completion;
        auto logged = warning_logs.size();
        const auto before = bus.statistics();
        const auto edges = pin_edges[pin];
        assert(bus.send_async_guarded(press, {}, completion, 1)); finish(completion);
        assert(completion.result.load() == cec::TxResult::Nack);
        assert(warning_logs.size() == logged + 1);
        assert(warning_logs.back() == "TX failed (GPIO 36, 3 bytes: 40 44 6d, result Nack, attempts 1, started true)");
        assert(bus.statistics().retries == before.retries && bus.statistics().tx_errors == before.tx_errors + 1);
        assert(pin_edges[pin] == edges + 11); // One start and ten header slots, then failed ACK.
        for (unsigned pass = 0; pass < 4000; ++pass) pump();
        assert(bus.idle() && pin_edges[pin] == edges + 11 && warning_logs.size() == logged + 1);

        logged = warning_logs.size();
        assert(bus.send_async_guarded(press, {}, completion, 3)); finish(completion);
        assert(completion.result.load() == cec::TxResult::Nack && warning_logs.size() == logged + 1);
        assert(warning_logs.back() == "TX failed (GPIO 36, 3 bytes: 40 44 6d, result Nack, attempts 3, started true)");
        assert(bus.statistics().retries == before.retries + 2); // Only the caller's budget controls attempts.

        logged = warning_logs.size();
        const auto errors = bus.statistics().tx_errors;
        assert(bus.send_async_guarded(poll, {}, completion, 1)); finish(completion);
        assert(completion.result.load() == cec::TxResult::Nack);
        assert(warning_logs.size() == logged && bus.statistics().tx_errors == errors);
        assert(bus.send_async_guarded(broadcast, {}, completion, 1)); finish(completion);
        assert(completion.result.load() == cec::TxResult::Success && warning_logs.size() == logged);

        std::atomic<uint32_t> generation{1};
        assert(bus.send_async_guarded(press, {&generation, 1}, completion, 1)); generation.store(2);
        finish(completion);
        assert(completion.result.load() == cec::TxResult::Cancelled && warning_logs.size() == ++logged);
        assert(warning_logs.back() == "TX failed (GPIO 36, 3 bytes: 40 44 6d, result Cancelled, attempts 0, started false)");
        assert(bus.send_async_guarded(press, {nullptr, 0, nullptr, 0, now + 1}, completion, 1)); step();
        finish(completion);
        assert(completion.result.load() == cec::TxResult::Expired && warning_logs.size() == ++logged);
        assert(warning_logs.back() == "TX failed (GPIO 36, 3 bytes: 40 44 6d, result Expired, attempts 0, started false)");

        external_low |= mask; gpio_set_level(pin, 1); step();
        assert(bus.send_async_guarded(press, {}, completion, 1)); finish(completion);
        assert(completion.result.load() == cec::TxResult::Timeout && warning_logs.size() == ++logged);
        assert(warning_logs.back() == "TX failed (GPIO 36, 3 bytes: 40 44 6d, result Timeout, attempts 1, started false)");
        external_low &= ~mask; gpio_set_level(pin, 1); step();

        const auto arbitration_edges = pin_edges[pin];
        uint64_t first_edge_at = 0;
        before_tick = [&] {
            if (!first_edge_at && pin_edges[pin] > arbitration_edges) first_edge_at = now - 50;
            if (first_edge_at && now >= first_edge_at + 7600) {
                // Another initiator wins the header's second, one-valued bit.
                external_low |= mask;
                gpio_set_level(pin, (output_low & mask) == 0);
            }
        };
        assert(bus.send_async_guarded(press, {}, completion, 1)); finish(completion); before_tick = {};
        assert(completion.result.load() == cec::TxResult::ArbitrationLost && warning_logs.size() == ++logged);
        assert(warning_logs.back() == "TX failed (GPIO 36, 3 bytes: 40 44 6d, result ArbitrationLost, attempts 1, started true)");
        external_low &= ~mask; gpio_set_level(pin, 1); step();

        const auto line_error_edges = pin_edges[pin]; bool delayed_tick = false;
        before_tick = [&] {
            if (!delayed_tick && pin_edges[pin] > line_error_edges) { now += 250; delayed_tick = true; }
        };
        assert(bus.send_async_guarded(press, {}, completion, 1)); finish(completion); before_tick = {};
        ++logged;
        assert(delayed_tick && completion.result.load() == cec::TxResult::LineError && warning_logs.size() == logged);
        const auto gap_failure = warning_logs.back();
        assert(gap_failure.find("result LineError, attempts 1, started true, reason TickGap, at_us ") != std::string::npos);
        assert(gap_failure.find(", gap_us 300, elapsed_us 300, slot -1, byte -1, bit -1, within_us 300, line low)") != std::string::npos);

        // A completed attempt's diagnostic must survive later ISR ticks until
        // the task consumes its existing result slot. No task/ISR logging is
        // permitted inside the wire lock or its callback.
        const auto stale_result_edges = pin_edges[pin];
        bool delayed_snapshot = false;
        before_tick = [&] {
            if (!delayed_snapshot && pin_edges[pin] > stale_result_edges) { now += 250; delayed_snapshot = true; }
        };
        assert(bus.send_async_guarded(press, {}, completion, 1));
        pump();
        for (unsigned pass = 0; pass < 500 && !delayed_snapshot; ++pass) step();
        assert(delayed_snapshot && completion.result.load() == cec::TxResult::Pending);
        const auto failure_at = now;
        before_tick = {};
        for (unsigned pass = 0; pass < 100; ++pass) step();
        finish(completion);
        ++logged;
        assert(completion.result.load() == cec::TxResult::LineError && warning_logs.size() == logged);
        assert(warning_logs.back().find("at_us " + std::to_string(failure_at) + ", gap_us 300") != std::string::npos);

        // A peer can also keep a normally zero data bit low after the
        // transmitter releases it. Here every callback remains exactly 50us.
        const auto held_line_edges = pin_edges[pin];
        uint64_t held_line_start = 0;
        before_tick = [&] {
            if (!held_line_start && pin_edges[pin] > held_line_edges) held_line_start = now - 50;
            if (held_line_start && now >= held_line_start + 5900) {
                external_low |= mask;
                gpio_set_level(pin, (output_low & mask) == 0);
            }
        };
        assert(bus.send_async_guarded(press, {}, completion, 1)); finish(completion); before_tick = {};
        ++logged;
        assert(completion.result.load() == cec::TxResult::LineError && warning_logs.size() == logged);
        assert(warning_logs.back().find("reason LowLine, at_us ") != std::string::npos);
        assert(warning_logs.back().find(", gap_us 50, elapsed_us 6300, slot 0, byte 0, bit 0, within_us 1800, line low)") != std::string::npos);
        external_low &= ~mask; gpio_set_level(pin, 1); step();

        cec::Frame longest{}; longest.size = 16;
        for (unsigned i = 0; i < 16; ++i) longest.data[i] = i;
        assert(bus.send_async_guarded(longest, {}, completion, 1)); finish(completion);
        assert(completion.result.load() == cec::TxResult::Nack && warning_logs.size() == ++logged);
        assert(warning_logs.back() == "TX failed (GPIO 36, 16 bytes: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f, result Nack, attempts 1, started true)");
    }
    assert(!timer && tasks.empty() && !output_low && !external_low);
}

static void begin_rejection_diagnostics_tests() {
    cec::Bus bus; assert(bus.start(36) == ESP_OK);
    const cec::Frame broadcast{{0x4f, 0x36}, 2};
    cec::TxCompletion completion;
    assert(bus.send_async_guarded(broadcast, {}, completion, 1));
    // Simulate a stopped callback source. The worker's existing timeout does
    // not reset the still-pending Engine, so another begin must be diagnosed
    // separately from either active-wire LineError path.
    timer->running = false;
    finish(completion);
    assert(completion.result.load() == cec::TxResult::Timeout);
    assert(bus.send_async_guarded(broadcast, {}, completion, 1));
    finish(completion);
    assert(completion.result.load() == cec::TxResult::LineError);
    assert(warning_logs.back().find("result LineError, attempts 0, started false, reason BeginRejected, at_us ") != std::string::npos);
    assert(warning_logs.back().find(", gap_us 0, elapsed_us 0, slot -1, byte -1, bit -1, within_us 0, line high)") != std::string::npos);
}

static void async_completion_contract_tests() {
    const cec::Frame broadcast{{0x4f,0x99},2}, press{{0x40,0x44,0x41},3};
    for(auto profile:{cec::Bus::QueueProfile::Legacy,cec::Bus::QueueProfile::Native}) {
        cec::Bus bus;assert(bus.start(28,profile)==ESP_OK);
        std::array<cec::TxCompletion,8> complete;
        const auto admitted_at=now;
        for(auto& tracked:complete) {
            tracked.result.store(cec::TxResult::Success);
            assert(bus.send_async_guarded(broadcast,{},tracked));
            assert(tracked.result.load()==cec::TxResult::Pending);
        }
        assert(now==admitted_at&&!bus.idle());
        cec::TxCompletion rejected;rejected.result.store(cec::TxResult::Nack);
        assert(!bus.send_async_guarded(broadcast,{},rejected));
        assert(rejected.result.load()==cec::TxResult::Nack&&now==admitted_at);
        bool refilled=false;
        before_tick=[&] {
            if(refilled||complete[0].result.load(std::memory_order_acquire)==cec::TxResult::Pending)return;
            refilled=true;
            assert(complete[0].result.load()==cec::TxResult::Success);
            assert(complete[1].result.load()==cec::TxResult::Pending);
            const auto refilled_at=now;
            // Publication guarantees capacity has already returned, even when
            // another frame is on the wire and still waiting for completion.
            assert(bus.send_async_guarded(broadcast,{},rejected));
            assert(rejected.result.load()==cec::TxResult::Pending&&now==refilled_at);
        };
        for(unsigned pass=0;pass<4000;++pass)pump();before_tick={};
        assert(refilled&&bus.idle()&&bus.statistics().transmitted==9);
        for(auto& tracked:complete)assert(tracked.result.load(std::memory_order_acquire)==cec::TxResult::Success);
        assert(rejected.result.load(std::memory_order_acquire)==cec::TxResult::Success);
        assert(!bus.send_async_guarded({}, {}, rejected)&&!bus.send_async_guarded(press,{},rejected,0));
        assert(rejected.result.load()==cec::TxResult::Success);
        assert(bus.send_async_guarded(press,{},rejected));finish(rejected);
        assert(rejected.result.load(std::memory_order_acquire)==cec::TxResult::Nack);
        std::atomic<uint32_t> counter{1};
        cec::ExecutionFence stale{&counter,2};
        cec::ExecutionFence unaligned{reinterpret_cast<const std::atomic<uint32_t>*>(reinterpret_cast<uintptr_t>(&counter)+1),1};
        assert(!bus.send_async_guarded(press,stale,rejected)&&!bus.send_async_guarded(press,unaligned,rejected));
        assert(rejected.result.load()==cec::TxResult::Nack);
    }
    assert(!timer&&tasks.empty()&&!output_low);
}

static void copied_frame_fifo_tests() {
    const cec::Frame first{{0x40, 0x99}, 2}, second{{0x4f, 0xaa}, 2};
    for (auto profile : {cec::Bus::QueueProfile::Legacy, cec::Bus::QueueProfile::Native}) {
        cec::Bus bus; assert(bus.start(32, profile) == ESP_OK);
        cec::TxCompletion one, two;
        // Admission copies caller-owned frame bytes; failure of the earlier
        // transmission never suppresses an independently admitted successor.
        for (unsigned run = 0; run < 16; ++run) {
            auto copy = first;
            assert(bus.send_async_guarded(copy, {}, one, 1));
            copy = second;
            assert(bus.send_async_guarded(copy, {}, two, 1));
            copy.size = 0;
            finish(one);
            assert(one.result.load() == cec::TxResult::Nack);
            assert(two.result.load() == cec::TxResult::Pending);
            finish(two);
            assert(two.result.load() == cec::TxResult::Success);
        }
        const auto sent_before = bus.statistics().transmitted;
        assert(bus.send(second));
        const auto admitted_at = now;
        assert(bus.send_async_guarded(first, {}, one, 1));
        assert(now == admitted_at && one.result.load() == cec::TxResult::Pending);
        finish(one);
        assert(one.result.load() == cec::TxResult::Nack);
        assert(bus.statistics().transmitted == sent_before + 1);
    }
    assert(!timer && tasks.empty() && !output_low);
}

static void receive_fifo_tests() {
    constexpr unsigned pin=37;
    constexpr uint64_t mask=uint64_t{1}<<pin;
    cec::Bus bus;assert(bus.start(pin)==ESP_OK);
    cec::Engine peer;peer.set_address(4);
    before_tick=[&]{
        const auto levels=uint64_t{GPIO.in}|(uint64_t{GPIO.in1.val}<<32);
        if(peer.tick(now,(levels&mask)!=0))external_low|=mask;else external_low&=~mask;
        GPIO.in=static_cast<uint32_t>(~(output_low|external_low));
        GPIO.in1.val=static_cast<uint32_t>(~(output_low|external_low)>>32);
    };
    std::array<cec::Frame,33> sent;
    for(size_t n=0;n<sent.size();++n){
        auto& frame=sent[n];frame.size=16;frame.data[0]=0x4f;
        for(size_t i=1;i<frame.size;++i)frame.data[i]=static_cast<uint8_t>(n+i);
        assert(peer.begin(frame,now));
        for(unsigned tick=0;tick<10000;++tick)step();
        assert(peer.take_result()==cec::TxResult::Success);
    }
    before_tick={};external_low=0;gpio_set_level(pin,1);
    assert(bus.statistics().received==32&&bus.statistics().rx_dropped==1);
    cec::Frame received;
    for(size_t n=0;n<32;++n){
        assert(bus.receive(received));
        assert(received.size==sent[n].size&&!std::memcmp(received.data,sent[n].data,received.size));
    }
    assert(!bus.receive(received));
}

static void full_fifo_tests() {
    const cec::Frame ordinary{{0x4f, 0x44, 1}, 3}, release{{0x4f, 0x45}, 2};
    for (auto profile : {cec::Bus::QueueProfile::Legacy, cec::Bus::QueueProfile::Native}) {
        cec::Bus bus; assert(bus.start(34, profile) == ESP_OK);
        const auto drain = [&] { for (unsigned pass = 0; pass < 4000; ++pass) pump(); };
        const auto before = bus.statistics().transmitted;
        for (unsigned i = 0; i < 8; ++i) assert(bus.send_guarded(ordinary, {}, 1));
        const auto full_at = now; const auto logged = warning_logs.size();
        assert(!bus.send(release));
        assert(!bus.send_guarded(ordinary, {}, 1));
        cec::TxCompletion rejected; rejected.result.store(cec::TxResult::Success);
        assert(!bus.send_async_guarded(release, {}, rejected, 1));
        assert(rejected.result.load() == cec::TxResult::Success && now == full_at);
        assert(warning_logs.size() == logged + 3);
        assert(warning_logs[logged] == "TX dropped: FIFO full (GPIO 34, 2 bytes: 4f 45)");
        assert(warning_logs[logged+1] == "TX dropped: FIFO full (GPIO 34, 3 bytes: 4f 44 01)");
        drain();
        assert(bus.statistics().transmitted == before + 8);
        assert(bus.statistics().tx_queue_high_water == 8);
        drain(); assert(bus.statistics().transmitted == before + 8); // No retry or backlog.

        // Independently admitted presses and release share the exact FIFO.
        // The last slot can hold either frame; there is no second backlog.
        cec::TxCompletion press_done, release_done;
        for (unsigned i = 0; i < 6; ++i) assert(bus.send_guarded(ordinary, {}, 1));
        assert(bus.send_async_guarded(ordinary, {}, press_done, 1));
        assert(bus.send_async_guarded(release, {}, release_done, 1));
        assert(!bus.send(release));
        finish(press_done); assert(release_done.result.load() == cec::TxResult::Pending);
        finish(release_done); assert(release_done.result.load() == cec::TxResult::Success);
        for (unsigned i = 0; i < 7; ++i) assert(bus.send_guarded(ordinary, {}, 1));
        assert(bus.send_async_guarded(ordinary, {}, press_done, 1));
        const auto release_logged = warning_logs.size();
        assert(!bus.send_async_guarded(release, {}, release_done, 1));
        assert(warning_logs.size() == release_logged + 1);
        assert(warning_logs.back() == "TX dropped: FIFO full (GPIO 34, 2 bytes: 4f 45)");
        drain();

        // The full sixteen-byte frame is present in its drop log.
        cec::Frame longest{}; longest.size = 16;
        for (unsigned i = 0; i < 16; ++i) longest.data[i] = i;
        for (unsigned i = 0; i < 8; ++i) assert(bus.send_guarded(ordinary, {}, 1));
        assert(!bus.send(longest));
        assert(warning_logs.back() == "TX dropped: FIFO full (GPIO 34, 16 bytes: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f)");
        drain();
    }
    assert(!timer && tasks.empty() && !output_low);
    // Total capacity includes a live first attempt on a stuck wire.
    cec::Bus active; assert(active.start(35, cec::Bus::QueueProfile::Native) == ESP_OK);
    external_low |= uint64_t{1} << 35; gpio_set_level(35, 1); step();
    assert(active.send_guarded(ordinary, {}, 1)); pump();
    for (unsigned i = 0; i < 7; ++i) assert(active.send_guarded(ordinary, {}, 1));
    const auto full_at = now;
    assert(!active.send_guarded(ordinary, {}, 1) && !active.send_guarded(release, {}, 1));
    assert(now == full_at && warning_logs.back() == "TX dropped: FIFO full (GPIO 35, 2 bytes: 4f 45)");
    external_low &= ~(uint64_t{1} << 35); gpio_set_level(35, 1);
    for (unsigned pass = 0; pass < 4000; ++pass) pump();
    assert(active.idle()); active.stop(); assert(!timer && tasks.empty() && !output_low);
}

static void independent_epoch_fence_tests() {
    std::atomic<uint32_t> route{1}, authority{1}, epoch{1};
    cec::ExecutionFence fence{&route,1,&authority,1};
    fence.tertiary_generation=&epoch;fence.expected_tertiary_generation=1;
    const cec::Frame frame{{0x4f,0xab},2};
    {
        cec::Bus bus;assert(bus.start(36,cec::Bus::QueueProfile::Native)==ESP_OK);
        const auto edges=pin_edges[36];cec::TxCompletion completion;
        // Changing only the third lifetime cancels queued work before dispatch.
        assert(bus.send_async_guarded(frame,fence,completion,1));epoch.store(2);
        finish(completion);assert(completion.result.load()==cec::TxResult::Cancelled&&pin_edges[36]==edges);
        // The same guard is checked after Engine::begin while waiting for a
        // free wire, before the actual first edge.
        external_low|=uint64_t{1}<<36;gpio_set_level(36,1);step();
        external_low&=~(uint64_t{1}<<36);gpio_set_level(36,1);step();
        fence.expected_tertiary_generation=2;const auto cancel_at=now+5000;
        before_tick=[&]{if(now>=cancel_at)epoch.store(3);};
        assert(bus.send_async_guarded(frame,fence,completion,1));finish(completion);
        before_tick={};assert(completion.result.load()==cec::TxResult::Cancelled&&pin_edges[36]==edges);
        // An already-started waveform finishes even if that lifetime changes.
        fence.expected_tertiary_generation=3;
        before_tick=[&]{if(pin_edges[36]>edges)epoch.store(4);};
        assert(bus.send_async_guarded(frame,fence,completion,1));finish(completion);
        before_tick={};assert(completion.result.load()==cec::TxResult::Success&&epoch.load()==4);
        assert(!bus.send_async_guarded(frame,fence,completion,1));
        fence.expected_tertiary_generation=4;
        auto unaligned=fence;
        unaligned.tertiary_generation=reinterpret_cast<const std::atomic<uint32_t>*>(reinterpret_cast<uintptr_t>(&epoch)+1);
        assert(!bus.send_async_guarded(frame,unaligned,completion,1));
        assert(bus.send_async_guarded(frame,fence,completion,1));finish(completion);
        assert(completion.result.load()==cec::TxResult::Success&&route.load()==1&&authority.load()==1);
        assert(bus.statistics().cancelled==2&&bus.statistics().transmitted==2&&bus.statistics().tx_errors==0);
    }
    assert(!timer&&tasks.empty()&&!output_low);
}

int main() {
    std::atomic<uint32_t> generation{7};
    std::atomic<uint32_t> key_generation{3};
    cec::ExecutionFence fence{&generation, 7};
    const cec::Frame f{{0x4f, 0x36}, 2};
    {
        cec::Bus a, b;
        assert(a.start(1) == ESP_OK && b.start(2) == ESP_OK);
        assert(timers_created == 1 && cec::Bus::runtime_statistics().active_ports == 2 && tasks.size()==1);
        assert(cec::Bus::runtime_statistics().timer_instances == 1);
        { // Reject a duplicate pin without changing the active line's latch.
            gpio_set_level(1, 0);
            const auto saved_low = output_low;
            cec::Bus duplicate;
            assert(duplicate.start(1) == ESP_ERR_INVALID_STATE && output_low == saved_low);
            gpio_set_level(1, 1); low_edges = 0;
        }
        // Cancel already-admitted queued operations before their worker runs.
        assert(a.send_guarded(f, fence)); assert(a.send_guarded(f, fence));
        generation.store(8); pump();
        assert(low_edges == 0 && a.statistics().cancelled == 2);
        assert(!a.send_guarded(f, fence));
        // A secondary hold/key counter rejects queued presses independently
        // of the still-valid route counter.
        cec::ExecutionFence key_fence{&generation, 8, &key_generation, 3};
        assert(a.send_guarded(f, key_fence));
        key_generation.store(4); pump();
        assert(low_edges == 0 && a.statistics().cancelled == 3);
        key_fence.expected_secondary_generation = 4;
        const auto key_cancel_at = now + 5000;
        before_tick = [&] { if (now >= key_cancel_at) key_generation.store(5); };
        cec::TxCompletion tracked;
        const auto key_admitted_at=now;
        assert(a.send_async_guarded(f,key_fence,tracked));
        assert(now==key_admitted_at&&tracked.result.load()==cec::TxResult::Pending);
        finish(tracked);assert(tracked.result.load(std::memory_order_acquire)==cec::TxResult::Cancelled);
        before_tick = {};
        assert(low_edges == 0 && a.statistics().cancelled == 4);
        // Cancel after Engine::begin, during the mandatory free-time wait.
        fence.expected_generation = 8;
        const auto cancel_at = now + 5000;
        before_tick = [&] { if (now >= cancel_at) generation.store(9); };
        assert(a.send_async_guarded(f,fence,tracked));finish(tracked);
        assert(tracked.result.load(std::memory_order_acquire)==cec::TxResult::Cancelled);
        before_tick = {};
        assert(low_edges == 0 && a.statistics().cancelled == 5 && a.statistics().tx_errors == 0);
        // Current generations still transmit normally through the real ISR.
        fence.expected_generation = 9;
        assert(a.send_async_guarded(f,fence,tracked));finish(tracked);
        assert(tracked.result.load(std::memory_order_acquire)==cec::TxResult::Success);
        assert(low_edges > 0 && a.statistics().transmitted == 1);
        // Invalidation after the first edge preserves that frame's waveform.
        const auto edges_before = low_edges;
        const auto inflight_cancel_at = now + 22000;
        before_tick = [&] { if (now >= inflight_cancel_at) generation.store(10); };
        assert(a.send_async_guarded(f,fence,tracked));finish(tracked);
        assert(tracked.result.load(std::memory_order_acquire)==cec::TxResult::Success);
        before_tick = {};
        assert(low_edges > edges_before && a.statistics().transmitted == 2);
    }
    assert(!timer && !output_low && tasks.empty());
    assert(cec::Bus::runtime_statistics().active_ports == 0 && cec::Bus::runtime_statistics().timer_instances == 0);
    {
        // Eight actual Bus instances share one dispatcher. A stuck line
        // remains pending while the other seven complete, without a task wait.
        std::array<cec::Bus, amity::max_cec_ports> lines;
        std::array<std::atomic<uint32_t>, amity::max_cec_ports> counters;
        std::array<cec::TxCompletion, amity::max_cec_ports> completed;
        for (unsigned i = 0; i < lines.size(); ++i) {
            counters[i].store(1);
            // Include the XIAO's GPIO44 to exercise the high GPIO bank.
            assert(lines[i].start(i + 1 == lines.size() ? 44 : 10 + i, cec::Bus::QueueProfile::Native) == ESP_OK);
        }
        assert(tasks.size() == 1 && cec::Bus::runtime_statistics().active_ports == amity::max_cec_ports);
        assert(cec::Bus::runtime_statistics().timer_instances == 1);
        external_low = (uint64_t{1} << 10) | (uint64_t{1} << 12); gpio_set_level(10, 1); step();
        external_low &= ~(uint64_t{1} << 12); gpio_set_level(12, 1); step(); // Fresh free-time interval before first edge.
        const auto cancelled_edges = pin_edges[11], expired_edges = pin_edges[12];
        for (unsigned i = 0; i < lines.size(); ++i) {
            const cec::ExecutionFence current{&counters[i], 1, nullptr, 0, i == 2 ? now + 5000 : 0};
            assert(lines[i].send_async_guarded(f, current, completed[i], 1));
        }
        counters[1].store(2); // Already queued; no first edge is permitted.
        for (unsigned pass = 0; pass < 400; ++pass) {
            pump();
            if (pin_edges[13]) counters[3].store(2); // In-flight waveform completes.
        }
        assert(completed[0].result.load() == cec::TxResult::Pending);
        assert(completed[1].result.load() == cec::TxResult::Cancelled);
        assert(completed[2].result.load() == cec::TxResult::Expired);
        for (unsigned i = 3; i < lines.size(); ++i)
            assert(completed[i].result.load() == cec::TxResult::Success);
        assert(pin_edges[11] == cancelled_edges && pin_edges[12] == expired_edges);
        for (unsigned pass = 0; pass < 1400; ++pass) pump();
        assert(completed[0].result.load() != cec::TxResult::Pending && completed[0].result.load() != cec::TxResult::Success);
        lines[0].stop(); external_low = 0; gpio_set_level(10, 1);
        assert(tasks.size() == 1 && cec::Bus::runtime_statistics().active_ports == amity::max_cec_ports - 1);
        std::array<cec::TxCompletion, 8> pending;
        for (auto& item : pending) assert(lines.back().send_async_guarded(f, {}, item, 1));
        cec::TxCompletion rejected;
        assert(!lines.back().send_async_guarded(f, {}, rejected, 1));
        for (unsigned pass = 0; pass < 1600; ++pass) pump();
        for (const auto& item : pending) assert(item.result.load() == cec::TxResult::Success);
    }
    assert(!timer && tasks.empty() && !output_low && cec::Bus::runtime_statistics().active_ports == 0);
    for (int budget : {0, 1, 2}) {
        allocation_budget = budget;
        cec::Bus missing;
        assert(missing.start(20, cec::Bus::QueueProfile::Native) == ESP_ERR_NO_MEM);
        missing.stop(); allocation_budget = -1;
        assert(!timer && tasks.empty());
        assert(missing.start(20, cec::Bus::QueueProfile::Native) == ESP_OK);
    }
    fail_task_creation = true;
    { cec::Bus missing; assert(missing.start(20, cec::Bus::QueueProfile::Native) == ESP_ERR_NO_MEM); }
    fail_task_creation = false;
    assert(!timer && tasks.empty() && cec::Bus::runtime_statistics().active_ports == 0);
    caller_attempt_tests();
    terminal_failure_diagnostics_tests();
    begin_rejection_diagnostics_tests();
    async_completion_contract_tests();
    copied_frame_fifo_tests();
    receive_fifo_tests();
    full_fifo_tests();
    independent_epoch_fence_tests();
    std::puts("Real Bus shared-timer lifecycle, queued/start-edge cancellation and in-flight completion passed");
    std::puts("Real Bus sole FIFO8, logged full drops, copied frames, caller budgets and independent completions passed");
}
