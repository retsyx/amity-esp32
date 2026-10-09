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
#include <string>
namespace device_settings {
// Called with amity_cfg initialized, before normal NVS/network initialization.
esp_err_t finish_factory_reset();
esp_err_t schedule_factory_reset();
bool reset_pending();
// Starts the optional active-low factory reset button after settings init.
// With CONFIG_AMITY_FW_RESET_BUTTON disabled, does not configure GPIO or tasks.
esp_err_t start_reset_button(int gpio);
// Load immutable factory settings and the separately persisted device overrides.
esp_err_t init();
const amity::FactoryCredentials *factory();
const char *hostname();
const char *initial_admin_auth();
uint16_t front_physical_address();
esp_err_t save_front_physical_address(const std::string &value); // Applies on reboot.
bool normalize_hostname(std::string &value);
esp_err_t save_hostname(std::string value); // Applies on reboot.
}
