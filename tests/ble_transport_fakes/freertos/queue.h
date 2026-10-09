/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "FreeRTOS.h"
struct TestQueue;
using QueueHandle_t = TestQueue *;
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
