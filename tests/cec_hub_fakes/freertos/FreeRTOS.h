/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <cstdint>
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = unsigned;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr unsigned portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
