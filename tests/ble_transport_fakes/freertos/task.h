/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "FreeRTOS.h"
using TaskFunction_t = void (*)(void *);
using TaskHandle_t = void *;
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, unsigned, TaskHandle_t *);
