/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "cec/engine.hpp"

namespace cec {
bool fence_current(ExecutionFence fence) {
    return (!fence.generation || fence.generation->load(std::memory_order_acquire) == fence.expected_generation) &&
        (!fence.secondary_generation ||
         fence.secondary_generation->load(std::memory_order_acquire) == fence.expected_secondary_generation) &&
        (!fence.tertiary_generation ||
         fence.tertiary_generation->load(std::memory_order_acquire) == fence.expected_tertiary_generation);
}
void Engine::set_address(uint8_t address) {
    auto replacement = policy_pending_ ? pending_policy_ : policy_;
    replacement.ack_address = address < 15 ? address : 15;
    install_receive_policy(replacement);
}

bool Engine::install_receive_policy(ReceivePolicy policy) {
    if (policy.ack_address > 15) return false;
    if (receiving_ || ack_drive_ || transmitting_) {
        pending_policy_ = policy;
        policy_pending_ = true;
    } else {
        policy_ = policy;
        policy_pending_ = false;
    }
    return true;
}

bool Engine::begin(const Frame &frame, uint64_t now, bool retry, ExecutionFence fence) {
    if (busy() || frame.size == 0 || frame.size > 16 || !fence_current(fence) ||
        (fence.expires_at_us && now >= fence.expires_at_us)) return false;
    tx_ = frame;
    tx_fence_ = fence;
    pending_ = true;
    tx_started_ = false;
    requested_ = now;
    free_time_ = retry ? 7200 : 16800;
    result_ = TxResult::Pending;
    return true;
}

void Engine::fail(TxResult result) {
    transmitting_ = pending_ = false;
    result_ = result;
    if (result == TxResult::ArbitrationLost) self_rx_ = false;
}

void Engine::receive_tick(uint64_t now, bool high) {
    if (ack_drive_ && now >= ack_until_) ack_drive_ = false;
    if (high != last_high_) {
        if (!high) {
            const auto period = now - falling_;
            if (receiving_) {
                const auto min = start_seen_ ? 4300u : 2050u;
                const auto max = start_seen_ ? 4700u : 2750u;
                if (period < min || period > max) receiving_ = false;
                else {
                    start_seen_ = false;
                    sampled_ = false;
                    if (rx_bit_ == 9 && rx_.size && !self_rx_ &&
                        (rx_.data[0] & 15) == policy_.ack_address && policy_.ack_address < 15) {
                        ack_drive_ = true;
                        ack_until_ = now + 1500;
                    }
                }
            }
            falling_ = now;
        } else {
            high_since_ = now;
            const auto low = now - falling_;
            if (low >= 3500 && low <= 3900) {
                receiving_ = start_seen_ = true;
                sampled_ = true;
                self_rx_ = transmitting_;
                rx_ = {};
                rx_bit_ = rx_byte_ = 0;
            } else if (receiving_ && !start_seen_ &&
                       !((low >= 350 && low <= 850) || (low >= 1250 && low <= 1750))) {
                receiving_ = false;
            }
        }
        last_high_ = high;
    }
    if (receiving_ && now - falling_ > (start_seen_ ? 4800u : 2850u)) {
        receiving_ = false;
        ack_drive_ = false;
    }
    if (!receiving_ || start_seen_ || sampled_ || now - falling_ < 1050) return;
    sampled_ = true;
    if (rx_bit_ < 8) {
        rx_byte_ = static_cast<uint8_t>((rx_byte_ << 1) | (high ? 1 : 0));
        if (rx_bit_ == 7) {
            if (rx_.size == 16) { receiving_ = false; return; }
            rx_.data[rx_.size++] = rx_byte_;
            rx_byte_ = 0;
        }
    } else if (rx_bit_ == 8) eom_ = high;
    else if (eom_) {
        if (!self_rx_ && rx_.size &&
            (policy_.observe_all || (rx_.data[0] & 15) == policy_.ack_address || (rx_.data[0] & 15) == 15)) {
            received_.frame = rx_;
            received_.local_rx_sequence = ++rx_sequence_;
            received_.local_monotonic_us = now;
            received_.addressed_to_local_identity = policy_.ack_address < 15 &&
                (rx_.data[0] & 15) == policy_.ack_address;
            rx_ready_ = true;
        }
        receiving_ = false;
    }
    rx_bit_ = static_cast<uint8_t>((rx_bit_ + 1) % 10);
}

bool Engine::tick(uint64_t now, bool high) {
    // Timing discontinuities (for example an unexpectedly masked interrupt)
    // invalidate a frame instead of emitting a malformed continuation.
    if (last_tick_ && now - last_tick_ > 200) {
        receiving_ = ack_drive_ = false;
        if (transmitting_) fail(TxResult::LineError);
    }
    last_tick_ = now;
    receive_tick(now, high);
    if (policy_pending_ && !receiving_ && !ack_drive_ && !transmitting_) {
        policy_ = pending_policy_;
        policy_pending_ = false;
    }
    if (pending_) {
        // Recheck at the actual first wire edge, after arbitrary bus free-time
        // waits. In-flight frames finish normally; they cannot be unsent.
        if (tx_fence_.expires_at_us && now >= tx_fence_.expires_at_us) fail(TxResult::Expired);
        else if (!fence_current(tx_fence_)) fail(TxResult::Cancelled);
        else if (now - requested_ > 1000000) fail(TxResult::Timeout);
        else if (high && !receiving_ && now - high_since_ >= free_time_) {
            pending_ = false;
            transmitting_ = true;
            tx_started_ = true;
            tx_start_ = now;
            tx_slot_ = -1;
            tx_sampled_ = false;
        }
    }
    if (!transmitting_) return ack_drive_;
    const auto elapsed = now - tx_start_;
    if (elapsed < 4500) return elapsed < 3700;
    // A frame is bounded to < 400 ms. Keep division 32-bit so ISR execution
    // cannot introduce a flash-resident 64-bit compiler runtime helper.
    const auto data_elapsed = static_cast<uint32_t>(elapsed - 4500);
    const auto slot = static_cast<int>(data_elapsed / 2400);
    const auto within = data_elapsed % 2400;
    if (slot >= tx_.size * 10) {
        fail(TxResult::Success);
        return ack_drive_;
    }
    if (slot != tx_slot_) { tx_slot_ = slot; tx_sampled_ = false; }
    const auto bit = slot % 10;
    const auto byte = slot / 10;
    const bool one = bit < 8 ? ((tx_.data[byte] >> (7 - bit)) & 1) :
                     bit == 8 ? byte + 1 == tx_.size : true;
    if (within >= 1050 && !tx_sampled_) {
        tx_sampled_ = true;
        if (bit == 9) {
            const bool broadcast = (tx_.data[0] & 15) == 15;
            if (high != broadcast) { fail(TxResult::Nack); return false; }
        } else if (one && !high) {
            fail(TxResult::ArbitrationLost);
            return false;
        }
    }
    // Every bit must return high by 1.75 ms, including ACK. Detect a stuck
    // line before proceeding to the next slot.
    if (within >= 1800 && !high) { fail(TxResult::LineError); return false; }
    return within < (one ? 600u : 1500u);
}

bool Engine::take_received(Frame &frame) {
    if (!rx_ready_) return false;
    frame = received_.frame;
    rx_ready_ = false;
    return true;
}
bool Engine::take_observation(ReceiveObservation &observation) {
    if (!rx_ready_) return false;
    observation = received_;
    rx_ready_ = false;
    return true;
}
TxResult Engine::take_result() {
    auto result = result_;
    result_ = TxResult::Pending;
    return result;
}
}
