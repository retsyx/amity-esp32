/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the device_settings.hpp functions that components/device_settings/reset_button.cpp defines.
#include "device_settings.hpp"
#include "seams.hpp"

esp_err_t device_settings::start_reset_button(int gpio) { return AMITY_SEAM(device_settings::start_reset_button, gpio); }
void device_settings::set_setup_button_handler(SetupButtonHandler handler, void *context) {
    AMITY_SEAM(device_settings::set_setup_button_handler, handler, context);
}
