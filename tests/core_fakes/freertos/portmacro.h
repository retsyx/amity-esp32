/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The port types, tick rate and critical sections of the Amity build
// (CONFIG_FREERTOS_HZ 100, so pdMS_TO_TICKS(5) is 0).
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t StackType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define configTICK_RATE_HZ 100
#define configSTACK_DEPTH_TYPE uint32_t
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define pdMS_TO_TICKS(xTimeInMs) ((TickType_t)(((TickType_t)(xTimeInMs) * (TickType_t)configTICK_RATE_HZ) / (TickType_t)1000U))
#define pdTICKS_TO_MS(xTicks) ((TickType_t)((uint64_t)(xTicks) * 1000 / configTICK_RATE_HZ))
#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)
#define pdPASS (pdTRUE)
#define pdFAIL (pdFALSE)
#define errQUEUE_EMPTY ((BaseType_t)0)
#define errQUEUE_FULL ((BaseType_t)0)
#define errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY (-1)

// Critical sections share one recursive lock; each mux counts its own nesting.
// A FreeRTOS call that can wait, made inside one, fails the scenario.
typedef struct {
    uint32_t owner;
    uint32_t count;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0xB33FFFFF, 0}
void vPortEnterCritical(portMUX_TYPE *mux);
void vPortExitCritical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL(mux) vPortExitCritical(mux)
#define portENTER_CRITICAL_ISR(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL_ISR(mux) vPortExitCritical(mux)
#define portENTER_CRITICAL_SAFE(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL_SAFE(mux) vPortExitCritical(mux)
// pdFALSE unless amity_fake::in_isr(true).
BaseType_t xPortInIsrContext(void);
#define portYIELD_FROM_ISR(...) ((void)0)

// Storage for a static queue or semaphore's control block; its handle is
// this object's address, as on the target.
typedef struct xSTATIC_QUEUE {
    void *pvDummy[16];
} StaticQueue_t;
typedef StaticQueue_t StaticSemaphore_t;

#ifdef __cplusplus
}
#endif
