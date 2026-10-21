/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#define IRAM_ATTR
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_INVALID_ARG = 2, ESP_ERR_NO_MEM = 3;
using BaseType_t = int;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_ISR(p) ((void)(p))
#define portEXIT_CRITICAL_ISR(p) ((void)(p))
using QueueHandle_t = void *;
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
QueueHandle_t xQueueCreate(unsigned, size_t);
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
void xQueueReset(QueueHandle_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
inline void *heap_caps_malloc(size_t size, unsigned caps) {
    if (caps != (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) std::abort();
    return std::malloc(size);
}
inline void heap_caps_free(void *p) { std::free(p); }
extern const void *external_pointer;
inline bool esp_ptr_internal(const void *p) { return p != external_pointer; }
inline bool esp_ptr_byte_accessible(const void *) { return true; }
int64_t esp_timer_get_time();
using gpio_num_t = int;
constexpr int GPIO_NUM_NC = -1, GPIO_MODE_INPUT_OUTPUT_OD = 1, GPIO_PULLUP_DISABLE = 0, GPIO_PULLDOWN_DISABLE = 0;
#define GPIO_IS_VALID_OUTPUT_GPIO(p) ((p) >= 0 && (p) <= 48 && (p) != 46)
struct gpio_config_t { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en; };
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(gpio_num_t, int);
struct Gpio {};
extern Gpio GPIO;
int gpio_ll_get_level(Gpio *, gpio_num_t);
void gpio_ll_set_level(Gpio *, gpio_num_t, int);
struct FakeTimer;
using gptimer_handle_t = FakeTimer *;
struct gptimer_alarm_event_data_t {};
using TimerCallback = bool (*)(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *);
struct gptimer_event_callbacks_t { TimerCallback on_alarm = nullptr; };
struct gptimer_config_t {
    int clk_src, direction, resolution_hz, intr_priority;
    struct { bool intr_shared = false; } flags;
};
struct gptimer_alarm_config_t { uint64_t alarm_count, reload_count; struct { bool auto_reload_on_alarm = false; } flags; };
constexpr int GPTIMER_CLK_SRC_DEFAULT = 0, GPTIMER_COUNT_UP = 0;
esp_err_t gptimer_new_timer(const gptimer_config_t *, gptimer_handle_t *);
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t, const gptimer_event_callbacks_t *, void *);
esp_err_t gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t *);
esp_err_t gptimer_enable(gptimer_handle_t);
esp_err_t gptimer_disable(gptimer_handle_t);
esp_err_t gptimer_start(gptimer_handle_t);
esp_err_t gptimer_stop(gptimer_handle_t);
esp_err_t gptimer_del_timer(gptimer_handle_t);
