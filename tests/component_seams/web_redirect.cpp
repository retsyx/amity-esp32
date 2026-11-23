/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/web/redirect.cpp.
#include "web.hpp"
#include "seams.hpp"

esp_err_t web::start_redirect(uint16_t port) { return AMITY_SEAM(web::start_redirect, port); }
esp_err_t web::stop_redirect() { return AMITY_SEAM(web::stop_redirect); }
std::string web::redirect_location(std::string_view head) { return AMITY_SEAM(web::redirect_location, head); }
