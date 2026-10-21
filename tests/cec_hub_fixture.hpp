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
#include "coordinator_intent.hpp"
#include "cec/bus.hpp"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <array>

struct FakeQueue { unsigned capacity; size_t item_size; std::deque<std::vector<unsigned char>> items; };
struct FakeSemaphore { unsigned capacity, count; bool pump_owner = false; };
struct FakeTask { TaskFunction_t entry; void *context; };
struct StopWorker {};
static FakeTask *latest_task;
static int worker_receives = -1;
static bool fixture_present = false;
static std::function<bool(int)> fixture_completion_ready;
static std::function<bool(int, const cec::Frame&)> fixture_submit_admission;
static std::function<cec::TxResult(int, const cec::Frame&)> fixture_wait_outcome;
static bool fixture_driver_paused = false;
static bool fixture_wire_deadlines = false; // Direct LiveSystem tests supply their own clock.
static int64_t fixture_time = 1000000;
static int64_t fixture_tick = 100000;
static int idle_passes = -1;
static std::vector<unsigned> idle_waits;
[[maybe_unused]] static std::vector<unsigned> command_waits;
static std::function<void()> fixture_wire_complete;
static std::function<void(int, const cec::Frame&)> fixture_before_start;
static std::function<void()> fixture_delay;
static bool fixture_late_reply = false;
static std::vector<cec::Frame> fixture_waited;
static bool in_wire_attempt = false;
struct SentFrame { int gpio; cec::Frame frame; };
static std::vector<SentFrame> fixture_sent;
static std::vector<SentFrame> fixture_transmitted;
struct SubmittedFrame { int gpio; cec::Frame frame; unsigned attempts; bool allow_overflow = false; };
[[maybe_unused]] static std::vector<SubmittedFrame> fixture_submitted;
struct GuardedFrame { int gpio; cec::Frame frame; cec::ExecutionFence fence; unsigned attempts = 0; bool allow_overflow = false; };
static std::vector<GuardedFrame> fixture_guarded;
static std::deque<SentFrame> fixture_incoming;
static std::function<bool(int, const cec::Frame &)> fixture_send;
static std::function<cec::TxResult(int, const cec::Frame &)> fixture_transmit;
static std::function<esp_err_t(int)> fixture_start_bus;
static std::vector<int> fixture_stopped_buses;
namespace config_store {
static std::vector<RequestedSession> fixture_requested;
esp_err_t remember_requested_session(amity::system::ConsortiumId, amity::system::ZoneId zone, std::optional<amity::system::ActivityId> activity, std::string&) {
    fixture_requested.erase(std::remove_if(fixture_requested.begin(), fixture_requested.end(), [&](const auto& r) { return r.zone_id == zone; }), fixture_requested.end());
    if (activity) fixture_requested.push_back({zone, *activity}); return ESP_OK;
}
esp_err_t load_last_requested_sessions(amity::system::ConsortiumId, std::vector<RequestedSession>& out, std::string&) { out = fixture_requested; return ESP_OK; }
}

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
    command_waits.push_back(wait);
    std::memcpy(item, q->items.front().data(), q->item_size); q->items.pop_front();
    if (worker_receives > 0) --worker_receives;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { assert(q->items.empty()); delete q; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeSemaphore{1, 1}; }
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned capacity, unsigned initial) { return new FakeSemaphore{capacity, initial, true}; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait) {
    const bool waiting_for_reply = !s->count && wait && s->pump_owner;
    if (!s->count && wait && s->pump_owner && latest_task) {
        const auto previous = worker_receives, previous_idle = idle_passes;
        for (unsigned pass = 0; pass < 128 && !s->count; ++pass) {
            worker_receives = 1; idle_passes = 1;
            try { latest_task->entry(latest_task->context); assert(false); } catch (const StopWorker&) {}
        }
        worker_receives = previous; idle_passes = previous_idle;
    }
    // Model a finite caller wait expiring as the owner completes its bounded
    // irreversible work. The signaled actual result remains available.
    if (waiting_for_reply && wait != portMAX_DELAY && fixture_late_reply) {
        fixture_late_reply = false; return pdFALSE;
    }
    if (!s->count) return pdFALSE; --s->count; return pdTRUE;
}
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
namespace cec {
struct FixtureBatch { std::array<bool,9> started{}; };
struct FixtureQueued { Submission item; std::shared_ptr<FixtureBatch> batch; size_t index; };
struct Bus::Impl { int gpio = 0; uint8_t address = 15; std::deque<Frame> received; std::deque<Completion> completed; std::deque<FixtureQueued> queued; };
Bus::Bus() : impl_(new Impl) {}
Bus::~Bus() = default;
esp_err_t Bus::start(int gpio) {
    if (fixture_start_bus) { const auto error = fixture_start_bus(gpio); if (error != ESP_OK) return error; }
    impl_->gpio = gpio; return ESP_OK;
}
esp_err_t Bus::start(int gpio, QueueProfile) { return start(gpio); }
void Bus::stop() { fixture_stopped_buses.push_back(impl_->gpio); impl_->gpio = 0; impl_->address = 15; impl_->received.clear(); impl_->completed.clear(); impl_->queued.clear(); }
void Bus::set_address(uint8_t address) { impl_->address = address; }
bool Bus::send(const Frame &frame, uint32_t) {
    fixture_sent.push_back({impl_->gpio, frame});
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
TxResult Bus::transmit(const Frame &frame, unsigned) {
    fixture_transmitted.push_back({impl_->gpio, frame});
    if (fixture_transmit) return fixture_transmit(impl_->gpio, frame);
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
bool Bus::idle() const { return impl_->queued.empty(); }
bool Bus::send_and_wait(const Frame &frame, uint32_t timeout, unsigned) {
    assert(!in_wire_attempt); in_wire_attempt = true;
    fixture_waited.push_back(frame);
    const bool accepted = send(frame, timeout);
    // Input can change while the hub owner waits for the wire to finish.
    if (fixture_wire_complete) fixture_wire_complete();
    in_wire_attempt = false;
    return accepted;
}
static bool current_fence(ExecutionFence fence) {
    return (!fence.generation || fence.generation->load() == fence.expected_generation) &&
        (!fence.secondary_generation || fence.secondary_generation->load() == fence.expected_secondary_generation);
}
bool Bus::send_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout, unsigned attempts, bool allow_overflow) {
    if (!current_fence(fence)) return false;
    fixture_guarded.push_back({impl_->gpio, frame, fence, attempts, allow_overflow});
    return send(frame, timeout);
}
bool Bus::send_and_wait_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout, unsigned attempts, TxResult* outcome, bool allow_overflow) {
    if (fixture_before_start) fixture_before_start(impl_->gpio, frame);
    if (!current_fence(fence)) { if (outcome) *outcome = TxResult::Cancelled; return false; }
    fixture_guarded.push_back({impl_->gpio, frame, fence, attempts, allow_overflow});
    const bool admitted=send_and_wait(frame, timeout, attempts);
    const auto result=admitted?(fixture_wait_outcome?fixture_wait_outcome(impl_->gpio,frame):TxResult::Success):TxResult::Pending;
    if(outcome)*outcome=result;
    return admitted&&result!=TxResult::Cancelled&&result!=TxResult::Expired;
}
bool Bus::install_receive_policy(ReceivePolicy policy) { if (policy.ack_address > 15) return false; impl_->address = policy.ack_address; return true; }
bool Bus::receive_observation(ReceiveObservation& observation, uint32_t timeout) {
    if (!receive(observation.frame, timeout)) return false;
    observation.local_monotonic_us = fixture_time; return true;
}
bool Bus::submit_guarded(const Frame& frame, ExecutionFence fence, uint32_t token, unsigned attempts, bool allow_overflow) {
    Submission item{frame,fence,token,attempts};item.allow_overflow=allow_overflow;return submit_guarded_batch(&item,1);
}
static Completion fixture_emit(Bus& bus,int gpio,const FixtureQueued& work){
    auto result=TxResult::Cancelled;bool started=false;
    if(fixture_wire_deadlines&&work.item.fence.expires_at_us&&uint64_t(fixture_time)>=work.item.fence.expires_at_us)result=TxResult::Expired;
    else if(current_fence(work.item.fence)&&(!work.item.requires_previous_start||(work.index&&work.batch->started[work.index-1]))){
        started=true;fixture_submitted.push_back({gpio,work.item.frame,work.item.attempts,work.item.allow_overflow});
        result=work.item.frame.size==1?bus.transmit(work.item.frame,work.item.attempts):bus.send(work.item.frame)?TxResult::Success:TxResult::Nack;
    }
    work.batch->started[work.index]=started;
    return {work.item.operation_token,result,started};
}
bool Bus::submit_guarded_batch(const Submission* batch,size_t count){
    if(!batch||!count||count>9)return false;
    for(size_t i=0;i<count;++i)if((impl_->completed.size()+impl_->queued.size()+i>=8&&(!batch[i].allow_overflow||impl_->completed.size()+impl_->queued.size()+i>=9))||
        !batch[i].operation_token||!batch[i].attempts||!current_fence(batch[i].fence)||
        (fixture_wire_deadlines&&batch[i].fence.expires_at_us&&uint64_t(fixture_time)>=batch[i].fence.expires_at_us)||(batch[i].requires_previous_start&&!i)||
        (fixture_submit_admission&&!fixture_submit_admission(impl_->gpio,batch[i].frame)))return false;
    const auto state=std::make_shared<FixtureBatch>();
    for(size_t i=0;i<count;++i){FixtureQueued work{batch[i],state,i};
        if(fixture_driver_paused||!impl_->queued.empty())impl_->queued.push_back(work);
        else impl_->completed.push_back(fixture_emit(*this,impl_->gpio,work));
    }
    return true;
}
bool Bus::take_completion(Completion& completion) {
    if(impl_->completed.empty()&&!impl_->queued.empty()&&!fixture_driver_paused){
        const auto work=impl_->queued.front();impl_->queued.pop_front();impl_->completed.push_back(fixture_emit(*this,impl_->gpio,work));
    }
    if (impl_->completed.empty() || (fixture_completion_ready && !fixture_completion_ready(impl_->gpio))) return false; completion = impl_->completed.front(); impl_->completed.pop_front(); return true;
}
}
