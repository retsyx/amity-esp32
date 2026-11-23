/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for the web server functions of components/web/web.cpp.
#include "web.hpp"
#include "seams.hpp"

esp_err_t web::start(httpd_handle_t &handle, const std::string &hostname, const std::string &initial_auth) {
    return AMITY_SEAM(web::start, handle, hostname, initial_auth);
}
esp_err_t web::stop(httpd_handle_t handle) { return AMITY_SEAM(web::stop, handle); }
esp_err_t web::route(httpd_handle_t handle, const char *path, httpd_method_t method, Handler handler, void *context) {
    return AMITY_SEAM(web::route, handle, path, method, handler, context);
}
