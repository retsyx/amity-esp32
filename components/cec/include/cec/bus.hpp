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
class Bus {
public:
    Bus();
    ~Bus();
    // All ports share one nonblocking internal-memory dispatcher with
    // identical queue limits, timing, attempts and completion semantics.
    esp_err_t start(int gpio);
    // Owner has already quiesced callers; also used to unwind partial startup.
    void stop();
    void set_address(uint8_t address);
    bool install_receive_policy(ReceivePolicy policy);
    // Admission never waits for FIFO capacity or wire acknowledgement. One
    // FIFO per wire holds eight queued+active messages, including releases.
    // Rejection drops the submission without retaining it; FIFO-full drops log.
    bool send_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts = 3);
    // Caller-owned tracking for the same sole FIFO. Rejection
    // leaves the tracker unchanged. Admission sets Pending; terminal publication
    // uses release ordering after the FIFO slot is freed. Read with acquire
    // ordering before reusing it, and keep it alive until that publication.
    bool send_async_guarded(const Frame &frame, ExecutionFence fence, TxCompletion &completion,
                            unsigned attempts = 1);
    bool receive(Frame &frame, uint32_t timeout_ms = 0);
    Statistics statistics() const;
    bool idle() const;
    static RuntimeStatistics runtime_statistics();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
