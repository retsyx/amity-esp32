/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Created tasks are recorded, not started: a scenario runs or disposes of each
// one through amity_fake::tasks() and run_task(). A task that returns from its
// function fails the scenario, as it would crash the target.
#include "freertos/portmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tskTaskControlBlock;
typedef struct tskTaskControlBlock *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)
#define tskIDLE_PRIORITY ((UBaseType_t)0U)

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char *const pcName,
                                   const configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                                   UBaseType_t uxPriority, TaskHandle_t *const pvCreatedTask, const BaseType_t xCoreID);
static inline BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName,
                                     const configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                                     UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask) {
    return xTaskCreatePinnedToCore(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask,
                                   tskNO_AFFINITY);
}
// A null handle deletes the calling task, which never returns.
void vTaskDelete(TaskHandle_t xTaskToDelete);
// Runs the amity_fake::on_delay hook, then advances the scripted clock.
void vTaskDelay(const TickType_t xTicksToDelay);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
char *pcTaskGetName(TaskHandle_t xTaskToQuery);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);
uint32_t ulTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait);
BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify);
void vTaskNotifyGiveFromISR(TaskHandle_t xTaskToNotify, BaseType_t *pxHigherPriorityTaskWoken);
#define taskYIELD() ((void)0)

#ifdef __cplusplus
}
#endif
