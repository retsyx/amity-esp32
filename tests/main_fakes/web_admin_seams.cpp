/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/web_admin/web_admin.cpp.
#include "web_admin.hpp"
#include "seams.hpp"

esp_err_t web_admin::load(amity::SystemControl &hub, bool defer_configuration) {
    return AMITY_SEAM(web_admin::load, hub, defer_configuration);
}
esp_err_t web_admin::finish_configuration_activation() { return AMITY_SEAM(web_admin::finish_configuration_activation); }
bool web_admin::member_recovery_only() { return AMITY_SEAM(web_admin::member_recovery_only); }
esp_err_t web_admin::start(amity::SystemControl &hub, const char *hostname, const char *initial_auth) {
    return AMITY_SEAM(web_admin::start, hub, hostname, initial_auth);
}
esp_err_t web_admin::stop() { return AMITY_SEAM(web_admin::stop); }
