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
struct Completion { uint32_t operation_token = 0; TxResult result = TxResult::Pending; bool started = false; };
struct Submission {
    Frame frame;
    ExecutionFence fence{};
    uint32_t operation_token = 0;
    unsigned attempts = 3;
    // A caller-selected batch dependency; no message/opcode interpretation.
    bool requires_previous_start = false;
    bool allow_overflow = false;
};
class Bus {
public:
    // Eight normal queued+active messages, plus one caller-eligible overflow.
    enum class QueueProfile { Legacy, Native };
    Bus();
    ~Bus();
    esp_err_t start(int gpio);
    // Native agents reserve nine completion records including eligible overflow.
    // Their compact queues share one nonblocking internal-memory dispatcher.
    esp_err_t start(int gpio, QueueProfile);
    // Owner has already quiesced callers; also used to unwind partial startup.
    void stop();
    void set_address(uint8_t address);
    bool install_receive_policy(ReceivePolicy policy);
    // A bounded wait provides producer backpressure when the TX queue fills.
    bool send(const Frame &frame, uint32_t timeout_ms = 0);
    // Cancellation before the first edge emits no frame; cancellation after
    // that edge preserves the current waveform and prevents stale retries.
    // fence.generation must be internal RAM and outlive all admitted work.
    bool send_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms = 0, unsigned attempts = 3,
                      bool allow_overflow = false);
    // Queue in FIFO order and wait for the requested wire attempts to finish. True means
    // admitted/completed, not acknowledged; failures are counted in statistics.
    bool send_and_wait(const Frame &frame, uint32_t timeout_ms = 0, unsigned attempts = 1);
    // As above, but false also reports cancellation before wire start. Other
    // wire outcomes are completion, not proof of ACK/equipment state.
    bool send_and_wait_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms = 0, unsigned attempts = 1,
                               TxResult* result = nullptr, bool allow_overflow = false);
    TxResult transmit(const Frame &frame, unsigned attempts = 3);
    TxResult transmit_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts = 3);
    // Completion metadata includes the single caller-eligible overflow slot.
    bool submit_guarded(const Frame&, ExecutionFence, uint32_t operation_token, unsigned attempts = 3,
                        bool allow_overflow = false);
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
