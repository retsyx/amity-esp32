/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include "factory_credentials.hpp"
#include "physical_address.hpp"
#include "amity/hardware_limits.hpp"
#include <string>
#include <array>
#include <vector>
namespace device_settings {
// Called with amity_cfg initialized, before normal NVS/network initialization.
esp_err_t finish_factory_reset();
esp_err_t schedule_factory_reset();
bool reset_pending();
// Starts the optional active-low factory reset button after settings init.
// With CONFIG_AMITY_FW_RESET_BUTTON disabled, does not configure GPIO or tasks.
esp_err_t start_reset_button(int gpio);
// Register before starting the button. The handler runs on its worker after
// release; the existing ten-second durable reset remains owned by this driver.
using SetupButtonHandler = void (*)(bool consortium_setup, void *context);
void set_setup_button_handler(SetupButtonHandler, void *context=nullptr);
// Load immutable factory settings and the separately persisted device overrides.
esp_err_t init();
const amity::FactoryCredentials *factory();
const char *hostname();
const char *initial_admin_auth();
uint16_t front_physical_address();
esp_err_t save_front_physical_address(const std::string &value);
// Stable hardware slots, never a compact list of attached adapters. The owner
// selects the stored scanned address or durable default on its task.
constexpr std::array<uint16_t, amity::max_tentacles> default_adapter_physical_addresses() {
    std::array<uint16_t, amity::max_tentacles> result{};
    for(auto& address:result)address=default_front_physical_address;
    return result;
}
struct AdapterDefaults {
    uint64_t revision = 1;
    std::array<uint16_t, amity::max_tentacles> front_physical = default_adapter_physical_addresses();
};
struct AdapterDefaultChange { uint8_t tentacle_id; uint16_t front_physical; };
AdapterDefaults adapter_defaults();
// One atomic NVS blob for the complete batch. Unchanged slots are retained.
esp_err_t save_adapter_defaults(uint64_t expected_revision, const std::vector<AdapterDefaultChange>&);
bool normalize_hostname(std::string &value);
esp_err_t save_hostname(std::string value); // Applies on reboot.
// Headless candidate enrollment only. Commit first, then update the in-memory
// name; the caller applies it to the network interface before connecting.
esp_err_t apply_enrollment_hostname(std::string value);
}
