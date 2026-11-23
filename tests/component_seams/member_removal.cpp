/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/member_removal.cpp.
#include "member_removal.hpp"
#include "seams.hpp"

esp_err_t system_services::prepare_removed_device_reset(amity::SystemControl &control,
                                                        amity::system::OctopusId authenticated_coordinator) {
    return AMITY_SEAM(system_services::prepare_removed_device_reset, control, authenticated_coordinator);
}
void system_services::removed_device_reset_response_attempted() {
    AMITY_SEAM(system_services::removed_device_reset_response_attempted);
}
