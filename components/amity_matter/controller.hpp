/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The Matter controller logic that needs no CHIP SDK, in controller.cpp: the
// enable and revocation latch, the enrollment check, state publication, input
// listing and key dispatch. amity_matter.cpp holds the CHIP glue. It shares
// the state below and implements chip_port, the SDK calls the logic makes.
#include "amity_matter.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace amity_matter {

// The attributes of the Amity endpoints that are reported when they change.
enum class Attribute { power, current_input, input_list, mute, volume, playback_state };

extern amity_matter_callbacks_t callbacks;
extern amity_matter_state_t state;
// Filled from the saved configuration for each use on the CHIP task.
extern amity_matter_input_t listed_inputs[AMITY_MATTER_MAX_INPUTS];
extern std::atomic<bool> started, stack_started, controller_enabled;
extern std::mutex lifecycle_mutex;

// Reports a changed attribute while the controller runs.
void report(Attribute attribute);
esp_err_t dispatch_key(uint8_t key);
// Inputs are read from the saved configuration when used, never retained.
size_t current_inputs();

// The CHIP SDK calls the controller logic makes, one SDK operation each.
namespace chip_port {
// Queues work for the CHIP task, which runs it holding the stack lock; false
// when the task refuses it.
bool schedule(void (*work)(intptr_t));
// Tells subscribers that an attribute changed. Needs the stack lock.
void report(Attribute attribute);
// Runs fn holding the stack lock.
void with_stack_lock(void (*fn)());
// The commissioning state, read on the CHIP task.
bool fail_safe_disarmed();
bool has_fabric();
bool station_provisioned();
bool station_connected();
// Commits the commissioned Wi-Fi configuration, on the CHIP task.
bool commit_wifi();
// The controller's user services, stopped holding the stack lock.
void stop_dns_sd();
void unregister_attribute_access();
void shutdown_server();
}  // namespace chip_port

}  // namespace amity_matter
