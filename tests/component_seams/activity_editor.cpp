/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/web_admin/activity_editor.cpp.
#include "activity_editor.hpp"
#include "seams.hpp"

bool web_admin::activity_editor_request(amity::SystemControl &hub, httpd_req_t *request, const std::string &path,
                                        esp_err_t &result) {
    return AMITY_SEAM(web_admin::activity_editor_request, hub, request, path, result);
}
