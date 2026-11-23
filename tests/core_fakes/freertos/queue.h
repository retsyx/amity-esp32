/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Queues copy items into a ring, in the caller's storage for a static queue.
// A wait that cannot succeed at once follows amity_fake::waits(): by default
// it advances the scripted clock, firing timers, until the item arrives or the
// wait times out; a wait that could only end through another thread fails.
#include "freertos/portmacro.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

struct QueueDefinition;
typedef struct QueueDefinition *QueueHandle_t;

#define queueSEND_TO_BACK ((BaseType_t)0)
#define queueSEND_TO_FRONT ((BaseType_t)1)
#define queueOVERWRITE ((BaseType_t)2)
#define queueQUEUE_TYPE_BASE ((uint8_t)0U)

QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize, const uint8_t ucQueueType);
QueueHandle_t xQueueGenericCreateStatic(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize,
                                        uint8_t *pucQueueStorage, StaticQueue_t *pxStaticQueue, const uint8_t ucQueueType);
#define xQueueCreate(uxQueueLength, uxItemSize) xQueueGenericCreate((uxQueueLength), (uxItemSize), queueQUEUE_TYPE_BASE)
#define xQueueCreateStatic(uxQueueLength, uxItemSize, pucQueueStorage, pxQueueBuffer) \
    xQueueGenericCreateStatic((uxQueueLength), (uxItemSize), (pucQueueStorage), (pxQueueBuffer), queueQUEUE_TYPE_BASE)
BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *const pvItemToQueue, TickType_t xTicksToWait,
                             const BaseType_t xCopyPosition);
#define xQueueSend(xQueue, pvItemToQueue, xTicksToWait) \
    xQueueGenericSend((xQueue), (pvItemToQueue), (xTicksToWait), queueSEND_TO_BACK)
#define xQueueSendToBack(xQueue, pvItemToQueue, xTicksToWait) \
    xQueueGenericSend((xQueue), (pvItemToQueue), (xTicksToWait), queueSEND_TO_BACK)
#define xQueueSendToFront(xQueue, pvItemToQueue, xTicksToWait) \
    xQueueGenericSend((xQueue), (pvItemToQueue), (xTicksToWait), queueSEND_TO_FRONT)
#define xQueueOverwrite(xQueue, pvItemToQueue) xQueueGenericSend((xQueue), (pvItemToQueue), 0, queueOVERWRITE)
BaseType_t xQueueGenericSendFromISR(QueueHandle_t xQueue, const void *const pvItemToQueue,
                                    BaseType_t *const pxHigherPriorityTaskWoken, const BaseType_t xCopyPosition);
#define xQueueSendFromISR(xQueue, pvItemToQueue, pxHigherPriorityTaskWoken) \
    xQueueGenericSendFromISR((xQueue), (pvItemToQueue), (pxHigherPriorityTaskWoken), queueSEND_TO_BACK)
BaseType_t xQueueReceive(QueueHandle_t xQueue, void *const pvBuffer, TickType_t xTicksToWait);
BaseType_t xQueuePeek(QueueHandle_t xQueue, void *const pvBuffer, TickType_t xTicksToWait);
UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t xQueue);
UBaseType_t uxQueueSpacesAvailable(const QueueHandle_t xQueue);
BaseType_t xQueueGenericReset(QueueHandle_t xQueue, BaseType_t xNewQueue);
#define xQueueReset(xQueue) xQueueGenericReset((xQueue), pdFALSE)
void vQueueDelete(QueueHandle_t xQueue);

#ifdef __cplusplus
}
#endif
