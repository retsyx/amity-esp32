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
#include <string>
namespace system_services {
void start_log_capture();
// Poll from a single task; logs after firmware readiness and every ten minutes.
void log_memory_if_due();
std::string logs();
size_t log_size();
esp_err_t upload_firmware(httpd_req_t *req);
void reboot_later();
esp_err_t confirm_firmware();
// Public boot-health metadata; contains no configuration or credentials.
const char *boot_id();
bool firmware_ready();
}
