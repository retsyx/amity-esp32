/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/group_firmware_update.cpp.
#include "group_firmware_update.hpp"
#include "seams.hpp"

void system_services::updates::start_group_updates(amity::system::OctopusId local, CohortHooks hooks) {
    AMITY_SEAM(system_services::updates::start_group_updates, local, std::move(hooks));
}
esp_err_t system_services::updates::begin_system_upload(const ReleaseManifest &release, std::string &error) {
    return AMITY_SEAM(system_services::updates::begin_system_upload, release, error);
}
void system_services::updates::system_upload_progress(uint64_t bytes) {
    AMITY_SEAM(system_services::updates::system_upload_progress, bytes);
}
esp_err_t system_services::updates::finish_system_upload(std::string &error) {
    return AMITY_SEAM(system_services::updates::finish_system_upload, error);
}
void system_services::updates::fail_system_upload(const std::string &error) {
    AMITY_SEAM(system_services::updates::fail_system_upload, error);
}
std::string system_services::updates::cohort_status_json() {
    return AMITY_SEAM(system_services::updates::cohort_status_json);
}
uint32_t system_services::updates::poll_group_update() { return AMITY_SEAM(system_services::updates::poll_group_update); }
void system_services::updates::wait_group_update(uint32_t milliseconds) {
    AMITY_SEAM(system_services::updates::wait_group_update, milliseconds);
}
