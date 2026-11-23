/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/system_services.cpp.
#include "system_services.hpp"
#include "seams.hpp"

void system_services::start_log_capture() { AMITY_SEAM(system_services::start_log_capture); }
void system_services::log_memory_if_due() { AMITY_SEAM(system_services::log_memory_if_due); }
std::string system_services::logs() { return AMITY_SEAM(system_services::logs); }
size_t system_services::log_size() { return AMITY_SEAM(system_services::log_size); }
amity::consortium::LogSnapshot system_services::log_snapshot() { return AMITY_SEAM(system_services::log_snapshot); }
esp_err_t system_services::upload_firmware(httpd_req_t *req) { return AMITY_SEAM(system_services::upload_firmware, req); }
void system_services::reboot_later() { AMITY_SEAM_AS(void (*)(), system_services::reboot_later); }
void system_services::reboot_later(uint32_t delay_ms) {
    AMITY_SEAM_AS(void (*)(uint32_t), system_services::reboot_later, delay_ms);
}
esp_err_t system_services::confirm_firmware() { return AMITY_SEAM(system_services::confirm_firmware); }
const char *system_services::boot_id() { return AMITY_SEAM(system_services::boot_id); }
bool system_services::firmware_ready() { return AMITY_SEAM(system_services::firmware_ready); }
