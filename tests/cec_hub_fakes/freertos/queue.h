/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include <cstddef>
struct FakeQueue;
using QueueHandle_t = FakeQueue *;
QueueHandle_t xQueueCreate(unsigned capacity, size_t item_size);
int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks);
void vQueueDelete(QueueHandle_t queue);
