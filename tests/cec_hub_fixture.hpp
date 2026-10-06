#pragma once
// These bindings execute the real hub_service.cpp admission/worker code.
// The scheduler is deterministic: tests pause the worker to fill its queue,
// then execute exactly one command to check reservation ownership transfer.
#include "amity/hub_service.hpp"
#include "cec/bus.hpp"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

struct FakeQueue { unsigned capacity; size_t item_size; std::deque<std::vector<unsigned char>> items; };
struct FakeSemaphore { unsigned capacity, count; };
struct FakeTask { TaskFunction_t entry; void *context; };
struct StopWorker {};
static FakeTask *latest_task;
static int worker_receives = -1;
static bool fixture_present = false;
static int64_t fixture_time = 1000000;
static int64_t fixture_tick = 100000;
static int idle_passes = -1;
static std::vector<unsigned> idle_waits;
static std::function<void()> fixture_wire_complete;
static std::vector<cec::Frame> fixture_waited;
static bool in_wire_attempt = false;
struct SentFrame { int gpio; cec::Frame frame; };
static std::vector<SentFrame> fixture_sent;
static std::deque<SentFrame> fixture_incoming;

QueueHandle_t xQueueCreate(unsigned capacity, size_t item_size) { return new FakeQueue{capacity, item_size, {}}; }
int xQueueSend(QueueHandle_t q, const void *item, unsigned) {
    if (q->items.size() == q->capacity) return pdFALSE;
    auto *bytes = static_cast<const unsigned char *>(item);
    q->items.emplace_back(bytes, bytes + q->item_size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *item, unsigned wait) {
    if (worker_receives == 0) throw StopWorker{};
    if (q->items.empty()) {
        if (idle_passes == 0) throw StopWorker{};
        if (idle_passes > 0) --idle_passes;
        idle_waits.push_back(wait);
        return pdFALSE;
    }
    std::memcpy(item, q->items.front().data(), q->item_size); q->items.pop_front();
    if (worker_receives > 0) --worker_receives;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { assert(q->items.empty()); delete q; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeSemaphore{1, 1}; }
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned capacity, unsigned initial) { return new FakeSemaphore{capacity, initial}; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned) { if (!s->count) return pdFALSE; --s->count; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { if (s->count == s->capacity) return pdFALSE; ++s->count; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
int xTaskCreate(TaskFunction_t entry, const char *, unsigned, void *context, unsigned, TaskHandle_t *task) {
    *task = latest_task = new FakeTask{entry, context}; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { if (latest_task == task) latest_task = nullptr; delete task; }
void vTaskDelay(unsigned ticks) { fixture_time += ticks * 1000; }
int64_t esp_timer_get_time() { return fixture_time += fixture_tick; }
[[maybe_unused]] static void run_one_command() {
    worker_receives = 1;
    try { latest_task->entry(latest_task->context); assert(false); } catch (const StopWorker &) {}
    worker_receives = -1;
}
[[maybe_unused]] static void run_idle(unsigned iterations) {
    idle_passes = static_cast<int>(iterations); idle_waits.clear();
    try { latest_task->entry(latest_task->context); assert(false); } catch (const StopWorker &) {}
    idle_passes = -1;
}
namespace cec {
struct Bus::Impl { int gpio = 0; uint8_t address = 15; std::deque<Frame> received; };
Bus::Bus() : impl_(new Impl) {}
Bus::~Bus() = default;
esp_err_t Bus::start(int gpio) { impl_->gpio = gpio; return ESP_OK; }
void Bus::set_address(uint8_t address) { impl_->address = address; }
bool Bus::send(const Frame &frame, uint32_t) {
    fixture_sent.push_back({impl_->gpio, frame});
    const uint8_t peer = impl_->gpio == 1 ? 0 : 4;
    if (!fixture_present || frame.size < 2 || frame.destination() != peer) return true;
    Frame reply;
    switch (frame.data[1]) {
    case 0x83:
        reply.size = 5; reply.data[0] = (peer << 4) | 15; reply.data[1] = 0x84;
        reply.data[2] = peer ? 0x10 : 0; reply.data[3] = 0; reply.data[4] = peer ? 4 : 0;
        break;
    case 0x46: {
        const char *name = peer ? "Source" : "TV";
        reply.data[0] = (peer << 4) | impl_->address; reply.data[1] = 0x47; reply.size = 2;
        while (*name) reply.data[reply.size++] = *name++;
        break;
    }
    case 0x8c:
        reply.size = 5; reply.data[0] = (peer << 4) | 15; reply.data[1] = 0x87;
        break;
    default: return true;
    }
    impl_->received.push_back(reply);
    return true;
}
TxResult Bus::transmit(const Frame &frame, unsigned) {
    return fixture_present && frame.destination() == (impl_->gpio == 1 ? 0 : 4) ? TxResult::Success : TxResult::Nack;
}
bool Bus::receive(Frame &frame, uint32_t) {
    for (auto i = fixture_incoming.begin(); i != fixture_incoming.end(); ++i) {
        if (i->gpio != impl_->gpio) continue;
        frame = i->frame; fixture_incoming.erase(i); return true;
    }
    if (impl_->received.empty()) return false;
    frame = impl_->received.front(); impl_->received.pop_front(); return true;
}
Statistics Bus::statistics() const { return {}; }
bool Bus::idle() const { return true; }
bool Bus::send_and_wait(const Frame &frame, uint32_t timeout, unsigned) {
    assert(!in_wire_attempt); in_wire_attempt = true;
    fixture_waited.push_back(frame);
    const bool accepted = send(frame, timeout);
    // Input can change while the hub owner waits for the wire to finish.
    if (fixture_wire_complete) fixture_wire_complete();
    in_wire_attempt = false;
    return accepted;
}
}
