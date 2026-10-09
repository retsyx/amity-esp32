/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "freertos/FreeRTOS.h"
struct FakeTask;
using TaskHandle_t = FakeTask *;
using TaskFunction_t = void (*)(void *);
int xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack, void *context, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(unsigned ticks);
