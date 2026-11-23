/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// esp_timer on the scripted clock. Timers fire, in deadline order, on the
// thread that advances the clock past their deadline (amity_fake::advance_us).
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
// The Amity build has no ISR dispatch.
typedef enum {
    ESP_TIMER_TASK,
    ESP_TIMER_MAX,
} esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *create_args, esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period);
esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
// Waits for a callback running on another thread to finish.
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t timeout_ticks);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
bool esp_timer_is_active(esp_timer_handle_t timer);
esp_err_t esp_timer_get_expiry_time(esp_timer_handle_t timer, uint64_t *expiry);

#ifdef __cplusplus
}
#endif
