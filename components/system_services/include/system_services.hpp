/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include "amity/consortium/logs.hpp"
#include <string>
#include <cstdint>
namespace system_services {
void start_log_capture();
// Poll from one task; drain allocation-failure events, and log memory totals
// after firmware readiness and every ten minutes.
void log_memory_if_due();
std::string logs();
size_t log_size();
// Stable bounded snapshot in PSRAM; a null snapshot reports unavailable memory.
amity::consortium::LogSnapshot log_snapshot();
esp_err_t download_logs(httpd_req_t *req,amity::system::OctopusId local);
// Revoke new downloads and finish async request ownership before HTTPD stops.
void stop_log_downloads();
esp_err_t upload_firmware(httpd_req_t *req);
void reboot_later();
void reboot_later(uint32_t delay_ms);
esp_err_t confirm_firmware();
// Public boot-health metadata; contains no configuration or credentials.
const char *boot_id();
bool firmware_ready();
}
