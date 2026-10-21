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
struct Statistics { uint32_t transmitted = 0, received = 0, tx_errors = 0, rx_dropped = 0; };
struct TxCompletion { std::atomic<TxResult> result{TxResult::Pending}; };
class Bus {
public:
    Bus();
    ~Bus();
    esp_err_t start(int gpio);
    void set_address(uint8_t address);
    // One FIFO per wire: eight queued+active messages, including releases.
    // Admission always returns immediately; full-FIFO drops are logged.
    bool send(const Frame &frame);
    bool send_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts = 3);
    // Admit immediately without waiting for a wire attempt or acknowledgement.
    // Rejection leaves completion unchanged. Admission sets it to Pending; the
    // worker publishes a terminal result with release ordering after freeing its
    // FIFO slot. Read result with acquire ordering before reusing the tracker.
    // Caller owns completion until terminal publication, and must not submit it
    // again, destroy it, or destroy this Bus while that work is outstanding.
    bool send_async_guarded(const Frame &frame, ExecutionFence fence, TxCompletion &completion,
                            unsigned attempts = 1);
    bool receive(Frame &frame, uint32_t timeout_ms = 0);
    Statistics statistics() const;
    bool idle() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
