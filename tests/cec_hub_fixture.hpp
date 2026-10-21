/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// These bindings execute the real hub_service.cpp admission/worker code.
// The scheduler is deterministic: tests pause the worker to fill its queue,
// then execute exactly one command to check reservation ownership transfer.
#include "amity/hub_service.hpp"
#include "cec/bus.hpp"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
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
static std::function<void()> fixture_delay;
static std::vector<cec::Frame> fixture_completed;
struct SentFrame { int gpio; cec::Frame frame; unsigned attempts = 3; bool allow_overflow = false; uint32_t timeout_ms = 0; };
static std::vector<SentFrame> fixture_sent;
static std::vector<SentFrame> fixture_transmitted;
static std::deque<SentFrame> fixture_incoming;
static std::function<bool(int, const cec::Frame &)> fixture_send;
static std::function<bool(int, const cec::Frame &)> fixture_admission;
static std::function<cec::TxResult(int, const cec::Frame &)> fixture_transmit;
struct PendingFrame {
    const cec::Bus *bus;
    int gpio;
    cec::Frame frame;
    cec::ExecutionFence fence;
    cec::TxCompletion *completion;
};
static bool fixture_async_autocomplete = true;
static std::deque<PendingFrame> fixture_pending;
[[maybe_unused]] static bool fixture_complete_next(cec::TxResult result = cec::TxResult::Pending, int gpio = 0);
[[maybe_unused]] static void fixture_complete_all(cec::TxResult result = cec::TxResult::Pending);

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
        if (fixture_delay) fixture_delay();
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
void vTaskDelay(unsigned ticks) { fixture_time += ticks * 1000; if (fixture_delay) fixture_delay(); }
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
[[maybe_unused]] static void fixture_finish_discovery(amity::HubService &hub, uint32_t after_generation = 0) {
    for (unsigned pass = 0; pass < 2000; ++pass) {
        const auto state = hub.snapshot();
        if (state.discovery_generation > after_generation && !state.discovering) return;
        if (!fixture_tick) fixture_time += 10000;
        run_idle(1);
    }
    assert(false && "incremental discovery did not complete");
}
namespace cec {
struct Bus::Impl { int gpio = 0; uint8_t address = 15; std::deque<Frame> received; };
Bus::Bus() : impl_(new Impl) {}
Bus::~Bus() {
    // Bus teardown abandons queued work just as deleting the real worker does.
    fixture_pending.erase(std::remove_if(fixture_pending.begin(), fixture_pending.end(),
        [&](const auto &pending) { return pending.bus == this; }), fixture_pending.end());
}
esp_err_t Bus::start(int gpio) { impl_->gpio = gpio; return ESP_OK; }
void Bus::set_address(uint8_t address) { impl_->address = address; }
bool Bus::send(const Frame &frame) {
    fixture_sent.push_back({impl_->gpio, frame, 3, false, 0});
    if (fixture_admission && !fixture_admission(impl_->gpio,frame)) return false;
    if (fixture_send) return fixture_send(impl_->gpio, frame);
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
bool Bus::receive(Frame &frame, uint32_t) {
    for (auto i = fixture_incoming.begin(); i != fixture_incoming.end(); ++i) {
        if (i->gpio != impl_->gpio) continue;
        frame = i->frame; fixture_incoming.erase(i); return true;
    }
    if (impl_->received.empty()) return false;
    frame = impl_->received.front(); impl_->received.pop_front(); return true;
}
Statistics Bus::statistics() const { return {}; }
bool Bus::idle() const {
    return std::none_of(fixture_pending.begin(), fixture_pending.end(),
        [&](const auto &pending) { return pending.bus == this; });
}
static bool current_fence(ExecutionFence fence) {
    return (!fence.generation || fence.generation->load()==fence.expected_generation) &&
        (!fence.secondary_generation || fence.secondary_generation->load()==fence.expected_secondary_generation);
}
bool Bus::send_guarded(const Frame& frame,ExecutionFence fence,unsigned attempts,bool allow_overflow) {
    if(!attempts||!current_fence(fence))return false;
    const auto index=fixture_sent.size();const bool accepted=send(frame);
    fixture_sent[index].attempts=attempts;fixture_sent[index].allow_overflow=allow_overflow;
    return accepted;
}
bool Bus::send_async_guarded(const Frame& frame,ExecutionFence fence,TxCompletion& completion,unsigned attempts,bool allow_overflow) {
    if (!frame.size || frame.size > 16 || !attempts || !current_fence(fence)) return false;
    const auto index = fixture_sent.size();
    const bool accepted = send(frame);
    fixture_sent[index].attempts = attempts;
    fixture_sent[index].allow_overflow = allow_overflow;
    if (!accepted) return false;
    if (frame.size == 1) fixture_transmitted.push_back({impl_->gpio, frame, attempts, allow_overflow, 0});
    completion.result.store(TxResult::Pending, std::memory_order_relaxed);
    fixture_pending.push_back({this, impl_->gpio, frame, fence, &completion});
    if (fixture_async_autocomplete) fixture_complete_next(TxResult::Pending, impl_->gpio);
    return true;
}

}
static bool fixture_complete_next(cec::TxResult result, int gpio) {
    const auto next = std::find_if(fixture_pending.begin(), fixture_pending.end(),
        [&](const auto &pending) { return !gpio || pending.gpio == gpio; });
    if (next == fixture_pending.end()) return false;
    const auto pending = *next;
    fixture_pending.erase(next); // Return the queue slot before terminal publication.
    if (!cec::current_fence(pending.fence)) result = cec::TxResult::Cancelled;
    else if (pending.fence.expires_at_us && uint64_t(fixture_time) >= pending.fence.expires_at_us) result = cec::TxResult::Expired;
    else {
        if (pending.frame.size > 1) {
            fixture_completed.push_back(pending.frame);
        }
        if (result == cec::TxResult::Pending) {
            if (pending.frame.size == 1)
                result = fixture_transmit ? fixture_transmit(pending.gpio, pending.frame) :
                    fixture_present && pending.frame.destination() == (pending.gpio == 1 ? 0 : 4) ?
                        cec::TxResult::Success : cec::TxResult::Nack;
            else result = cec::TxResult::Success;
        }
    }
    pending.completion->result.store(result, std::memory_order_release);
    return true;
}
static void fixture_complete_all(cec::TxResult result) {
    while (fixture_complete_next(result)) {}
}
