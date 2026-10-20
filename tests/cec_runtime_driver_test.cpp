/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime.hpp"
#include "cec/bus.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include <array>

struct Object { virtual ~Object() = default; };
struct Queue : Object { size_t capacity, item_size; std::deque<std::vector<uint8_t>> items; };
struct Semaphore : Object { unsigned available = 0; };
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
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *) { auto *s = new Semaphore; s->available = true; return s; }
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
void vSemaphoreDelete(SemaphoreHandle_t h) { delete static_cast<Object *>(h); }
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

static void caller_attempt_tests() {
    const cec::Frame press{{0x40, 0x44, 0x41}, 3}, power{{0x40, 0x44, 0x6d}, 3};
    const cec::Frame release{{0x40, 0x45}, 2}, query{{0x40, 0x8f}, 2}, poll{{0x44}, 1};
    std::atomic<uint32_t> generation{1};const cec::ExecutionFence fence{&generation,1};
    {
        cec::Bus bus;assert(bus.start(30)==ESP_OK);
        auto expect_attempts=[&](unsigned attempts,auto action){
            const auto before=bus.statistics();const auto edges=pin_edges[30];
            action();pump();
            const auto after=bus.statistics();
            assert(after.retries-before.retries==attempts-1&&pin_edges[30]>edges);
        };
        // No receiver ACKs this line. The real worker/Engine must stop after
        // exactly the caller-provided budget; no opcode policy belongs here.
        expect_attempts(3,[&]{assert(bus.send(press));});
        expect_attempts(3,[&]{assert(bus.send_guarded(press,fence,0,3));});
        expect_attempts(3,[&]{assert(bus.send_and_wait(press,0,3));});
        expect_attempts(3,[&]{assert(bus.send_and_wait_guarded(press,fence,0,3));});
        cec::TxResult outcome = cec::TxResult::Pending;
        expect_attempts(1,[&]{assert(bus.send_and_wait_guarded(press,fence,0,1,&outcome));});
        assert(outcome == cec::TxResult::Nack); // Admission and actual result are distinct.
        expect_attempts(3,[&]{assert(bus.transmit(press,3)==cec::TxResult::Nack);});
        expect_attempts(3,[&]{assert(bus.transmit_guarded(press,fence,3)==cec::TxResult::Nack);});
        expect_attempts(3,[&]{assert(bus.submit_guarded(press,fence,1,3));});
        cec::Completion completed;assert(bus.take_completion(completed)&&completed.operation_token==1&&completed.result==cec::TxResult::Nack);
        expect_attempts(8,[&]{assert(bus.send_guarded(power,fence,0,8));});
        expect_attempts(3,[&]{assert(bus.send(release));});
        expect_attempts(3,[&]{assert(bus.send_guarded(query,fence,0,3));});
        expect_attempts(3,[&]{assert(bus.transmit(poll,3)==cec::TxResult::Nack);}); // Header0x44 alone is a poll, not an opcode.
        const auto edges=pin_edges[30];
        assert(!bus.send_guarded(press,fence,0,0)&&!bus.send_and_wait(press,0,0)&&!bus.send_and_wait_guarded(press,fence,0,0));
        assert(!bus.submit_guarded(press,fence,2,0)&&bus.transmit(press,0)==cec::TxResult::LineError&&bus.transmit_guarded(press,fence,0)==cec::TxResult::LineError);
        pump();assert(pin_edges[30]==edges); // Invalid zero budget never becomes a send.
        const auto errors=bus.statistics().tx_errors;
        for(unsigned queued=0;queued<8;++queued)assert(bus.send(press));
        assert(!bus.send(press)); // Legacy and native TX FIFOs share the explicit eight-message bound.
        pump();assert(bus.statistics().tx_errors==errors+8&&bus.statistics().tx_queue_high_water==8);
    }
    {
        cec::Bus bus;assert(bus.start(31,cec::Bus::QueueProfile::Native)==ESP_OK);
        auto expect_attempts=[&](unsigned attempts,auto action){
            const auto before=bus.statistics();const auto edges=pin_edges[31];action();
            for(unsigned pass=0;pass<500;++pass)pump();
            assert(bus.statistics().retries-before.retries==attempts-1&&pin_edges[31]>edges);
        };
        expect_attempts(3,[&]{assert(bus.send(press));});
        expect_attempts(3,[&]{assert(bus.send_guarded(power,fence,0,3));});
        expect_attempts(3,[&]{assert(bus.submit_guarded(press,fence,3,3));});
        cec::Completion completed;assert(bus.take_completion(completed)&&completed.operation_token==3&&completed.result==cec::TxResult::Nack);
        expect_attempts(3,[&]{assert(bus.submit_guarded(release,fence,4,3));});
        assert(bus.take_completion(completed)&&completed.operation_token==4&&completed.result==cec::TxResult::Nack);
        expect_attempts(3,[&]{assert(bus.submit_guarded(query,fence,5,3));});
        assert(bus.take_completion(completed)&&completed.operation_token==5&&completed.result==cec::TxResult::Nack);
        expect_attempts(3,[&]{assert(bus.submit_guarded(poll,fence,6,3));});
        assert(bus.take_completion(completed)&&completed.operation_token==6&&completed.result==cec::TxResult::Nack);
    }
    assert(!timer&&tasks.empty()&&!output_low);
}

static void atomic_batch_tests() {
    const cec::Frame first{{0x40, 0x99}, 2}, second{{0x4f, 0xaa}, 2};
    for (auto profile : {cec::Bus::QueueProfile::Legacy, cec::Bus::QueueProfile::Native}) {
        cec::Bus bus; assert(bus.start(32, profile) == ESP_OK);
        const auto drain = [&] { for (unsigned i = 0; i < 4000; ++i) pump(); };
        std::atomic<uint32_t> generation{1};
        cec::Submission batch[2]{{first, {&generation, 1}, 101, 1}, {second, {}, 102, 3, true}};
        cec::Completion result;
        assert(!bus.submit_guarded_batch(nullptr, 1) && !bus.submit_guarded_batch(batch, 0) && !bus.submit_guarded_batch(batch, 10));
        auto invalid = batch[0]; invalid.requires_previous_start = true;
        assert(!bus.submit_guarded_batch(&invalid, 1));
        invalid = batch[1]; invalid.operation_token = 0;
        cec::Submission invalid_batch[2]{batch[0], invalid};
        const auto original_edges = pin_edges[32];
        assert(!bus.submit_guarded_batch(invalid_batch, 2)); pump();
        assert(pin_edges[32] == original_edges && !bus.take_completion(result));

        // Whole-batch admission: seven queued messages leave no room for two.
        for (unsigned i = 0; i < 7; ++i) assert(bus.send(second));
        assert(!bus.submit_guarded_batch(batch, 2));
        assert(bus.send(second) && !bus.send(second)); drain();
        assert(!bus.take_completion(result));

        // A never-started predecessor suppresses only its requested dependent.
        const auto canceled_edges = pin_edges[32];
        assert(bus.submit_guarded_batch(batch, 2)); generation.store(2); drain();
        assert(bus.take_completion(result) && result.operation_token == 101 && result.result == cec::TxResult::Cancelled && !result.started);
        assert(bus.take_completion(result) && result.operation_token == 102 && result.result == cec::TxResult::Cancelled && !result.started);
        assert(pin_edges[32] == canceled_edges);
        assert(bus.submit_guarded(second, {}, 103, 1)); drain();
        assert(bus.take_completion(result) && result.operation_token == 103 && result.result == cec::TxResult::Success && result.started);

        // Result failure is not a dependency failure: the first real edge is
        // what the caller requested, regardless of the predecessor's ACK.
        batch[0].fence.expected_generation = 2;
        assert(bus.submit_guarded_batch(batch, 2)); drain();
        assert(bus.take_completion(result) && result.operation_token == 101 && result.result == cec::TxResult::Nack && result.started);
        assert(bus.take_completion(result) && result.operation_token == 102 && result.result == cec::TxResult::Success && result.started);

        // Expiry during free-time after atomic admission also emits neither.
        external_low |= uint64_t{1} << 32; gpio_set_level(32, 1); step();
        external_low &= ~(uint64_t{1} << 32); gpio_set_level(32, 1); step();
        batch[0].fence.expires_at_us = now + 5000;
        const auto expired_edges = pin_edges[32];
        assert(bus.submit_guarded_batch(batch, 2)); drain();
        assert(bus.take_completion(result) && result.operation_token == 101 && result.result == cec::TxResult::Expired && !result.started);
        assert(bus.take_completion(result) && result.operation_token == 102 && result.result == cec::TxResult::Cancelled && !result.started);
        assert(pin_edges[32] == expired_edges); batch[0].fence.expires_at_us = 0;

        // Caller-owned stack frames are copied; metadata is bounded/reusable.
        for (unsigned run = 0; run < 16; ++run) {
            cec::Submission copied[2]{batch[0], batch[1]}; assert(bus.submit_guarded_batch(copied, 2));
            copied[0].frame.size = 0; copied[1].frame.size = 0; drain();
            assert(bus.take_completion(result) && result.operation_token == 101 && result.started);
            assert(bus.take_completion(result) && result.operation_token == 102 && result.started);
        }
        const auto sent_before = bus.statistics().transmitted;
        assert(bus.send(second));
        assert(bus.transmit(first, 1) == cec::TxResult::Nack);
        assert(bus.statistics().transmitted == sent_before + 1); // Synchronous calls do not bypass the FIFO.
    }
    assert(!timer && tasks.empty() && !output_low);

    // The eight-message bound includes an active message on the wire, even
    // for callers that do not reserve asynchronous completion records.
    cec::Bus active; assert(active.start(33, cec::Bus::QueueProfile::Native) == ESP_OK);
    external_low |= uint64_t{1} << 33; gpio_set_level(33, 1); step();
    assert(active.send(second)); pump();
    for (unsigned i = 0; i < 7; ++i) assert(active.send(second));
    assert(!active.send(second));
    external_low &= ~(uint64_t{1} << 33); gpio_set_level(33, 1);
    for (unsigned i = 0; i < 4000; ++i) pump();
    assert(active.idle()); active.stop();
    assert(!timer && tasks.empty() && !output_low);
}

static void overflow_slot_tests() {
    const cec::Frame ordinary{{0x4f, 0x44, 1}, 3}, release_shape{{0x4f, 0x45}, 2};
    // Deliberately not a UCR: permission is supplied by the caller, not inferred.
    const cec::Frame eligible{{0x4f, 0x99}, 2};
    for (auto profile : {cec::Bus::QueueProfile::Legacy, cec::Bus::QueueProfile::Native}) {
        cec::Bus bus;assert(bus.start(34, profile)==ESP_OK);
        const auto drain=[&]{for(unsigned pass=0;pass<4000;++pass)pump();};
        const auto before=bus.statistics().transmitted;
        for(unsigned i=0;i<8;++i)assert(bus.send_guarded(ordinary,{},0,1));
        assert(!bus.send(release_shape)); // An opcode cannot obtain the ninth slot.
        assert(bus.submit_guarded(eligible,{},900,1,true));
        const auto full_at=now;
        assert(!bus.submit_guarded(eligible,{},901,1,true)&&!bus.send_guarded(eligible,{},1000,1,true));
        cec::TxResult rejected=cec::TxResult::Success;
        assert(!bus.send_and_wait_guarded(eligible,{},1000,1,&rejected,true)&&rejected==cec::TxResult::Pending&&now==full_at);
        assert(!bus.send_guarded(ordinary,{},0,1));drain();
        cec::Completion completion;assert(bus.take_completion(completion)&&completion.operation_token==900&&completion.started);
        assert(!bus.take_completion(completion)&&bus.statistics().transmitted==before+9&&bus.statistics().tx_queue_high_water==9);
        drain();assert(bus.statistics().transmitted==before+9); // Dropped calls never become a retry/backlog.
        for(unsigned i=0;i<8;++i)assert(bus.send_guarded(ordinary,{},0,1));
        const auto wait_began=now;assert(bus.send_guarded(ordinary,{},1000,1));
        assert(now>wait_began);drain(); // Explicit ordinary producer backpressure remains available.

        // Ordered all-or-none admission: ordinary eighth + eligible ninth fits.
        cec::Submission pair[2]{{ordinary,{},910,1},{eligible,{},911,1,true,true}};
        for(unsigned i=0;i<7;++i)assert(bus.send_guarded(ordinary,{},0,1));
        assert(bus.submit_guarded_batch(pair,2));assert(!bus.send_guarded(eligible,{},0,1,true));drain();
        for(unsigned token=910;token<=911;++token)assert(bus.take_completion(completion)&&completion.operation_token==token&&completion.started);
        for(unsigned i=0;i<8;++i)assert(bus.send_guarded(ordinary,{},0,1));
        assert(!bus.submit_guarded_batch(pair,2)); // Ordinary UCP may never take the ninth slot.
        assert(bus.send_guarded(eligible,{},0,1,true));drain();assert(!bus.take_completion(completion));

        // Nine exact asynchronous completion reservations, no hidden tenth.
        std::array<cec::Submission,9> all{};
        for(unsigned i=0;i<all.size();++i)all[i]={ordinary,{},920+i,1,false,i==8};
        assert(bus.submit_guarded_batch(all.data(),all.size()));assert(!bus.submit_guarded(eligible,{},930,1,true));drain();
        for(unsigned token=920;token<=928;++token)assert(bus.take_completion(completion)&&completion.operation_token==token&&completion.started);
        assert(!bus.take_completion(completion));
    }
    assert(!timer&&tasks.empty()&&!output_low);
    // Total capacity includes a live/queued first attempt on a stuck wire.
    cec::Bus active;assert(active.start(35,cec::Bus::QueueProfile::Native)==ESP_OK);
    external_low|=uint64_t{1}<<35;gpio_set_level(35,1);step();assert(active.send_guarded(ordinary,{},0,1));pump();
    for(unsigned i=0;i<7;++i)assert(active.send_guarded(ordinary,{},0,1));
    assert(!active.send_guarded(ordinary,{},0,1)&&active.submit_guarded(eligible,{},940,1,true));
    assert(!active.submit_guarded(eligible,{},941,1,true));
    external_low&=~(uint64_t{1}<<35);gpio_set_level(35,1);for(unsigned pass=0;pass<4000;++pass)pump();
    cec::Completion completion;assert(active.take_completion(completion)&&completion.operation_token==940&&completion.started);
    assert(active.idle());active.stop();assert(!timer&&tasks.empty()&&!output_low);
}

int main() {
    std::atomic<uint32_t> generation{7};
    std::atomic<uint32_t> key_generation{3};
    cec::ExecutionFence fence{&generation, 7};
    const cec::Frame f{{0x4f, 0x36}, 2};
    {
        cec::Bus a, b;
        assert(a.start(1) == ESP_OK && b.start(2) == ESP_OK);
        assert(timers_created == 1 && cec::Bus::runtime_statistics().active_ports == 2);
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
        assert(!a.send_and_wait_guarded(f, key_fence));
        before_tick = {};
        assert(low_edges == 0 && a.statistics().cancelled == 4);
        // Cancel after Engine::begin, during the mandatory free-time wait.
        fence.expected_generation = 8;
        const auto cancel_at = now + 5000;
        before_tick = [&] { if (now >= cancel_at) generation.store(9); };
        assert(!a.send_and_wait_guarded(f, fence));
        before_tick = {};
        assert(low_edges == 0 && a.statistics().cancelled == 5 && a.statistics().tx_errors == 0);
        // Current generations still transmit normally through the real ISR.
        fence.expected_generation = 9;
        assert(a.send_and_wait_guarded(f, fence));
        assert(low_edges > 0 && a.statistics().transmitted == 1);
        // Invalidation after the first edge preserves that frame's waveform.
        const auto edges_before = low_edges;
        const auto inflight_cancel_at = now + 22000;
        before_tick = [&] { if (now >= inflight_cancel_at) generation.store(10); };
        assert(a.send_and_wait_guarded(f, fence));
        before_tick = {};
        assert(low_edges > edges_before && a.statistics().transmitted == 2);
    }
    assert(!timer && !output_low && tasks.empty());
    assert(cec::Bus::runtime_statistics().active_ports == 0 && cec::Bus::runtime_statistics().timer_instances == 0);
    {
        // Ten actual Bus instances share one native dispatcher. A stuck line
        // remains pending while the other nine complete, without a task wait.
        std::array<cec::Bus, 10> lines;
        std::array<std::atomic<uint32_t>, 10> counters;
        for (unsigned i = 0; i < lines.size(); ++i) {
            counters[i].store(1);
            assert(lines[i].start(10 + i, cec::Bus::QueueProfile::Native) == ESP_OK);
        }
        assert(tasks.size() == 1 && cec::Bus::runtime_statistics().active_ports == 10);
        assert(cec::Bus::runtime_statistics().timer_instances == 1);
        external_low = (uint64_t{1} << 10) | (uint64_t{1} << 12); gpio_set_level(10, 1); step();
        external_low &= ~(uint64_t{1} << 12); gpio_set_level(12, 1); step(); // Fresh free-time interval before first edge.
        const auto cancelled_edges = pin_edges[11], expired_edges = pin_edges[12];
        for (unsigned i = 0; i < lines.size(); ++i) {
            const cec::ExecutionFence current{&counters[i], 1, nullptr, 0, i == 2 ? now + 5000 : 0};
            assert(lines[i].submit_guarded(f, current, i + 1, 1));
        }
        counters[1].store(2); // Already queued; no first edge is permitted.
        for (unsigned pass = 0; pass < 400; ++pass) {
            pump();
            if (pin_edges[13]) counters[3].store(2); // In-flight waveform completes.
        }
        cec::Completion result;
        assert(!lines[0].take_completion(result));
        assert(lines[1].take_completion(result) && result.result == cec::TxResult::Cancelled);
        assert(lines[2].take_completion(result) && result.result == cec::TxResult::Expired);
        for (unsigned i = 3; i < lines.size(); ++i)
            assert(lines[i].take_completion(result) && result.operation_token == i + 1 && result.result == cec::TxResult::Success);
        assert(pin_edges[11] == cancelled_edges && pin_edges[12] == expired_edges);
        for (unsigned pass = 0; pass < 1400; ++pass) pump();
        assert(lines[0].take_completion(result) && result.result != cec::TxResult::Success);
        lines[0].stop(); external_low = 0; gpio_set_level(10, 1);
        assert(tasks.size() == 1 && cec::Bus::runtime_statistics().active_ports == 9);
        for (unsigned token = 20; token < 28; ++token) assert(lines[9].submit_guarded(f, {}, token, 1));
        assert(!lines[9].submit_guarded(f, {}, 28, 1)); // Eight exact completion reservations.
        for (unsigned pass = 0; pass < 1600; ++pass) pump();
        for (unsigned token = 20; token < 28; ++token)
            assert(lines[9].take_completion(result) && result.operation_token == token && result.result == cec::TxResult::Success);
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
    atomic_batch_tests();
    overflow_slot_tests();
    std::puts("Real Bus shared-timer lifecycle, queued/start-edge cancellation and in-flight completion passed");
    std::puts("Real Bus sole FIFO nominal8+eligible ninth, later drop, atomic batches, caller budgets and first-edge dependency passed");
}
