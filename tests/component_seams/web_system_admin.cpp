/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/web_admin/system_admin.cpp.
#include "system_admin.hpp"
#include "web_admin.hpp"
#include "seams.hpp"

esp_err_t web_admin::load_configuration(amity::SystemControl &hub, bool defer_configuration) {
    return AMITY_SEAM(web_admin::load_configuration, hub, defer_configuration);
}
bool web_admin::configuration_is_passive() { return AMITY_SEAM(web_admin::configuration_is_passive); }
esp_err_t web_admin::finish_configuration_activation() { return AMITY_SEAM(web_admin::finish_configuration_activation); }
bool web_admin::admin_request(httpd_req_t *request, const std::string &path, esp_err_t &result) {
    return AMITY_SEAM(web_admin::admin_request, request, path, result);
}
