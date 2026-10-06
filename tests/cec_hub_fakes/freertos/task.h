#pragma once
#include "freertos/FreeRTOS.h"
struct FakeTask;
using TaskHandle_t = FakeTask *;
using TaskFunction_t = void (*)(void *);
int xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack, void *context, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(unsigned ticks);
