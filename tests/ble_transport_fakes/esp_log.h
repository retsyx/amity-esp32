/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
void test_ble_log(bool warning, const char *format, ...);
#define ESP_LOGI(tag, ...) test_ble_log(false, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_ble_log(true, __VA_ARGS__)
