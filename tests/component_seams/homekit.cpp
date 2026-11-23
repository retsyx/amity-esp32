/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/amity_homekit/homekit.cpp.
#include "homekit.hpp"
#include "seams.hpp"

esp_err_t amity_homekit::start(amity::SystemControl &hub, const amity_matter_callbacks_t &callbacks) {
    return AMITY_SEAM(amity_homekit::start, hub, callbacks);
}
esp_err_t amity_homekit::stop() { return AMITY_SEAM(amity_homekit::stop); }
esp_err_t amity_homekit::rename_hostname(const char *hostname) { return AMITY_SEAM(amity_homekit::rename_hostname, hostname); }
auto amity_homekit::status() -> Status { return AMITY_SEAM(amity_homekit::status); }
esp_err_t amity_homekit::open_pairing() { return AMITY_SEAM(amity_homekit::open_pairing); }
esp_err_t amity_homekit::close_pairing() { return AMITY_SEAM(amity_homekit::close_pairing); }
esp_err_t amity_homekit::remove_pairings() { return AMITY_SEAM(amity_homekit::remove_pairings); }
