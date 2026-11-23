/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/firmware_update.cpp.
#include "firmware_update.hpp"
#include "seams.hpp"

void system_services::updates::maintenance_hooks(std::function<esp_err_t()> begin, std::function<void()> end) {
    AMITY_SEAM(system_services::updates::maintenance_hooks, std::move(begin), std::move(end));
}
esp_err_t system_services::updates::validate_release(std::string_view manifest, std::string_view signature_hex,
                                                     ReleaseManifest &release, std::string &error) {
    return AMITY_SEAM(system_services::updates::validate_release, manifest, signature_hex, release, error);
}
esp_err_t system_services::updates::enter_maintenance(std::string &error) {
    return AMITY_SEAM(system_services::updates::enter_maintenance, error);
}
void system_services::updates::leave_maintenance() { AMITY_SEAM(system_services::updates::leave_maintenance); }
