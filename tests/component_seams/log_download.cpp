/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/system_services/log_download.cpp.
#include "system_services.hpp"
#include "seams.hpp"

esp_err_t system_services::download_logs(httpd_req_t *req, amity::system::OctopusId local) {
    return AMITY_SEAM(system_services::download_logs, req, local);
}
void system_services::stop_log_downloads() { AMITY_SEAM(system_services::stop_log_downloads); }
