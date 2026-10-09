/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "freertos/FreeRTOS.h"
struct FakeSemaphore;
using SemaphoreHandle_t = FakeSemaphore *;
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned capacity, unsigned initial);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned ticks);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
