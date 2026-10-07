#pragma once
#include "FreeRTOS.h"
using TaskFunction_t = void (*)(void *);
using TaskHandle_t = void *;
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, unsigned, TaskHandle_t *);
