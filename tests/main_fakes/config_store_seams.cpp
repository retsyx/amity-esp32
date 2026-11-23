/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/config_store/config_store.cpp.
#include "config_store.hpp"
#include "seams.hpp"

esp_err_t config_store::init() { return AMITY_SEAM(config_store::init); }
esp_err_t config_store::read(const char *key, std::string &value) { return AMITY_SEAM(config_store::read, key, value); }
esp_err_t config_store::write(const char *key, const std::string &value) {
    return AMITY_SEAM(config_store::write, key, value);
}
