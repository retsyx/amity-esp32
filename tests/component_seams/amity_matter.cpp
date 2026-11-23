/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the amity_matter_* functions of components/amity_matter/amity_matter.cpp.
#include "amity_matter.h"
#include "seams.hpp"

esp_err_t amity_matter_set_controller_enabled(bool enabled) { return AMITY_SEAM(amity_matter_set_controller_enabled, enabled); }
esp_err_t amity_matter_start(const amity_matter_callbacks_t *callbacks) { return AMITY_SEAM(amity_matter_start, callbacks); }
bool amity_matter_controller_enrollment_complete(void) { return AMITY_SEAM(amity_matter_controller_enrollment_complete); }
esp_err_t amity_matter_update_state(const amity_matter_state_t *state) { return AMITY_SEAM(amity_matter_update_state, state); }
esp_err_t amity_matter_flush_state(void) { return AMITY_SEAM(amity_matter_flush_state); }
esp_err_t amity_matter_update_remote(bool paired, bool connected, int battery_percent, int charging) {
    return AMITY_SEAM(amity_matter_update_remote, paired, connected, battery_percent, charging);
}
esp_err_t amity_matter_get_status(amity_matter_status_t *status) { return AMITY_SEAM(amity_matter_get_status, status); }
esp_err_t amity_matter_open_commissioning(uint16_t seconds) { return AMITY_SEAM(amity_matter_open_commissioning, seconds); }
esp_err_t amity_matter_close_commissioning(void) { return AMITY_SEAM(amity_matter_close_commissioning); }
esp_err_t amity_matter_remove_fabric(uint8_t fabric_index) { return AMITY_SEAM(amity_matter_remove_fabric, fabric_index); }
