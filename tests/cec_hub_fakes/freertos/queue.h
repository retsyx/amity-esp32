#pragma once
#include "freertos/FreeRTOS.h"
#include <cstddef>
struct FakeQueue;
using QueueHandle_t = FakeQueue *;
QueueHandle_t xQueueCreate(unsigned capacity, size_t item_size);
int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks);
void vQueueDelete(QueueHandle_t queue);
