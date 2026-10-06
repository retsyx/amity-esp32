#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void *;
using TaskFunction_t = void (*)(void *);
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
