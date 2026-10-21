/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "amity/hub.hpp"
#include "esp_err.h"
#include <functional>
#include <memory>

namespace amity {
// Thread-safe facade. Commands are queued; snapshot returns the latest state.
// Callback runs on the hub task, outside the snapshot lock. It should be brief.
class HubService {
public:
    HubService();
    ~HubService();
    // Discover the front physical address on startup; use the supplied default
    // only if the back scan yields no usable physical-address report.
    esp_err_t start(int front_gpio, int back_gpio, uint16_t front_physical = 0x1000);
    bool configure(std::vector<Activity> activities, std::vector<Quirk> quirks = {});
    // Reserve command capacity before committing external state. The callback
    // runs synchronously on the caller, must return an error on failure, and
    // must not call configure recursively. A failed reservation never calls it.
    esp_err_t configure_with_commit(std::vector<Activity> activities, std::vector<Quirk> quirks,
                                    std::function<esp_err_t()> commit);
    // ESP_ERR_TIMEOUT means ignored during settling; same-target repair bypasses it.
    esp_err_t request_activity(int index);
    // Power-long repair does not enter Python's activity-selection input gate.
    esp_err_t repair_activity();
    // Advances on accepted selections/configuration, including same-target
    // selections, so input can observe them before queued execution completes.
    uint32_t activity_selection() const;
    int activity_target() const;
    bool select_activity(int index);
    // Each held key has its own completion-paced continuation. Submission of
    // other keys and commands never waits for a wire attempt or acknowledgement.
    bool press_key(uint8_t key, bool repeat = false);
    // The worker discards this press if the activity/configuration changed
    // after the originating operation captured its generation. repeat=true
    // starts a held stream with the same cancellation policy as press_key().
    bool press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat = false);
    uint32_t activity_generation() const;
    bool release_key(uint8_t key);
    // Release every held key (disconnect, input loss, or legacy callers).
    bool release_key();
    bool standby();
    bool discover();
    Snapshot snapshot() const;
    void on_change(std::function<void(const Snapshot &)> callback);
private:
    esp_err_t request_activity(int index, bool notify_selection);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
