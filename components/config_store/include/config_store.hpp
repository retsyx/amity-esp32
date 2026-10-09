/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include "nvs.h"
#include <string>
namespace config_store {
// Dedicated partition, independent of Matter's network/fabric storage. Never erase on error.
esp_err_t init();
esp_err_t read(const char *key, std::string &value);
esp_err_t write(const char *key, const std::string &value);
}
