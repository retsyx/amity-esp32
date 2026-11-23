/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// ESP-IDF additions. A task created with capabilities must be deleted with
// vTaskDeleteWithCaps, and any other task with vTaskDelete; the wrong one
// fails the scenario.
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t pvTaskCode, const char *const pcName,
                                           const configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                                           UBaseType_t uxPriority, TaskHandle_t *const pvCreatedTask,
                                           const BaseType_t xCoreID, UBaseType_t uxMemoryCaps);
static inline BaseType_t xTaskCreateWithCaps(TaskFunction_t pvTaskCode, const char *const pcName,
                                             configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                                             UBaseType_t uxPriority, TaskHandle_t *pvCreatedTask,
                                             UBaseType_t uxMemoryCaps) {
    return xTaskCreatePinnedToCoreWithCaps(pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority,
                                           pvCreatedTask, tskNO_AFFINITY, uxMemoryCaps);
}
// A null handle deletes the calling task, which never returns.
void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete);
QueueHandle_t xQueueCreateWithCaps(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, UBaseType_t uxMemoryCaps);
void vQueueDeleteWithCaps(QueueHandle_t xQueue);

#ifdef __cplusplus
}
#endif
