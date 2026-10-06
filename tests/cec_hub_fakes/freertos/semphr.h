#pragma once
#include "freertos/FreeRTOS.h"
struct FakeSemaphore;
using SemaphoreHandle_t = FakeSemaphore *;
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned capacity, unsigned initial);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned ticks);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
