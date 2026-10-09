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
using TickType_t = uint32_t;
constexpr BaseType_t pdFALSE = 0, pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr uint32_t portTICK_PERIOD_MS = 10;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>((ms) / portTICK_PERIOD_MS)
void test_yield_from_isr();
#define portYIELD_FROM_ISR() test_yield_from_isr()
