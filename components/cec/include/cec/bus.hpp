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
class Bus {
public:
    Bus();
    ~Bus();
    esp_err_t start(int gpio);
    void set_address(uint8_t address);
    // One FIFO per wire: eight queued+active messages and one caller-eligible
    // overflow slot. Eligible work never waits for capacity and drops at nine.
    // Ordinary callers may request bounded admission backpressure explicitly.
    bool send(const Frame &frame, uint32_t timeout_ms = 0);
    bool send_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms = 0, unsigned attempts = 3,
                      bool allow_overflow = false);
    // Queue in FIFO order and wait for the requested wire attempts to finish. True means
    // admitted/completed, not acknowledged; failures are counted in statistics.
    bool send_and_wait(const Frame &frame, uint32_t timeout_ms = 0, unsigned attempts = 1);
    bool send_and_wait_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms = 0, unsigned attempts = 1,
                              TxResult* result = nullptr, bool allow_overflow = false);
    TxResult transmit(const Frame &frame, unsigned attempts = 3);
    TxResult transmit_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts = 3);
    bool receive(Frame &frame, uint32_t timeout_ms = 0);
    Statistics statistics() const;
    bool idle() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
