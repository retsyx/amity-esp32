#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct fake_timer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *);
    void *arg;
    int dispatch_method;
    const char *name;
} esp_timer_create_args_t;
#define ESP_TIMER_TASK 0
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t ticks);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
