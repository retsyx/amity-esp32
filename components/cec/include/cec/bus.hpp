/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "cec/engine.hpp"
#include "esp_err.h"
#include <memory>

namespace cec {
struct Statistics {
    uint32_t transmitted = 0, received = 0, tx_errors = 0, rx_dropped = 0;
    uint32_t tx_queue_high_water = 0, arbitration_losses = 0, line_errors = 0, retries = 0, cancelled = 0;
};
struct RuntimeStatistics {
    uint32_t ticks = 0, late_ticks = 0, max_tick_gap_us = 0, max_isr_us = 0;
    uint8_t active_ports = 0;
    uint8_t timer_instances = 0;
};
struct TxCompletion { std::atomic<TxResult> result{TxResult::Pending}; };
struct Completion { uint32_t operation_token = 0; TxResult result = TxResult::Pending; bool started = false; };
struct Submission {
    Frame frame;
    ExecutionFence fence{};
    uint32_t operation_token = 0;
    unsigned attempts = 3;
    // A caller-selected batch dependency; no message/opcode interpretation.
    bool requires_previous_start = false;
};
class Bus {
public:
    // Source compatibility only: every profile uses identical shared dispatch,
    // Engine behavior and eight total queued-plus-active slots.
    enum class QueueProfile { Legacy, Native };
    Bus();
    ~Bus();
    esp_err_t start(int gpio);
    // All ports share one nonblocking internal-memory dispatcher; the profile
    // does not change queue limits, timing, attempts or completion semantics.
    esp_err_t start(int gpio, QueueProfile);
    // Owner has already quiesced callers; also used to unwind partial startup.
    void stop();
    void set_address(uint8_t address);
    bool install_receive_policy(ReceivePolicy policy);
    // Admission never waits for FIFO capacity or wire acknowledgement. One
    // FIFO per wire holds eight queued+active messages, including releases.
    // Rejection drops the submission without retaining it; FIFO-full drops log.
    bool send(const Frame &frame);
    bool send_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts = 3);
    // Caller-owned tracking for legacy users of the same sole FIFO. Rejection
    // leaves the tracker unchanged. Admission sets Pending; terminal publication
    // uses release ordering after the FIFO slot is freed. Read with acquire
    // ordering before reusing it, and keep it alive until that publication.
    bool send_async_guarded(const Frame &frame, ExecutionFence fence, TxCompletion &completion,
                            unsigned attempts = 1);
    bool submit_guarded(const Frame&, ExecutionFence, uint32_t operation_token, unsigned attempts = 3);
    // Copy the whole batch into this wire's sole FIFO, or admit none. Generic
    // frame/fence/attempt metadata only; the caller chooses every message.
    bool submit_guarded_batch(const Submission*, size_t count);
    bool take_completion(Completion&);
    bool receive(Frame &frame, uint32_t timeout_ms = 0);
    // The Bus instance supplies the observation's physical-port context.
    bool receive_observation(ReceiveObservation &observation, uint32_t timeout_ms = 0);
    Statistics statistics() const;
    bool idle() const;
    static RuntimeStatistics runtime_statistics();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
