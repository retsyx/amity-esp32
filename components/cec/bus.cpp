/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "cec/bus.hpp"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include <cstdlib>

namespace cec {
struct Bus::Impl {
    static void *operator new(size_t size) {
        void *memory = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) std::abort();
        return memory;
    }
    static void operator delete(void *memory) { heap_caps_free(memory); }
    struct Work { Frame frame; SemaphoreHandle_t completed = nullptr; unsigned attempts = 3; };
    Engine engine;
    gpio_num_t gpio = GPIO_NUM_NC;
    gptimer_handle_t timer = nullptr;
    QueueHandle_t rx = nullptr, tx = nullptr, result = nullptr;
    SemaphoreHandle_t gate = nullptr;
    TaskHandle_t task = nullptr;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    Statistics stats;
    bool enabled = false, started = false;

    static bool IRAM_ATTR tick(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *arg);
    TxResult transmit(const Frame &frame, unsigned attempts) {
        if (!started || !attempts || !frame.size || frame.size > 16) return TxResult::LineError;
        if (xSemaphoreTake(gate, pdMS_TO_TICKS(5000)) != pdTRUE) return TxResult::Timeout;
        TxResult status = TxResult::Timeout;
        for (unsigned attempt = 0; attempt < attempts; ++attempt) {
            xQueueReset(result);
            portENTER_CRITICAL(&lock);
            const bool begun = engine.begin(frame, esp_timer_get_time(), attempt > 0);
            portEXIT_CRITICAL(&lock);
            if (!begun) break;
            if (xQueueReceive(result, &status, pdMS_TO_TICKS(1600)) != pdTRUE) status = TxResult::Timeout;
            if (status == TxResult::Success) break;
            // Polls intentionally have one attempt; directed messages retry.
        }
        portENTER_CRITICAL(&lock);
        if (status == TxResult::Success) ++stats.transmitted;
        else if (!(frame.size == 1 && status == TxResult::Nack)) ++stats.tx_errors;
        portEXIT_CRITICAL(&lock);
        xSemaphoreGive(gate);
        return status;
    }
    static void run(void *arg) {
        auto *p = static_cast<Impl *>(arg);
        Work work;
        for (;;) if (xQueueReceive(p->tx, &work, portMAX_DELAY) == pdTRUE) {
            p->transmit(work.frame, work.attempts);
            if (work.completed) xSemaphoreGive(work.completed);
        }
    }
    ~Impl() {
        if (task) vTaskDelete(task);
        if (started) gptimer_stop(timer);
        if (enabled) gptimer_disable(timer);
        if (timer) gptimer_del_timer(timer);
        if (gpio != GPIO_NUM_NC) gpio_set_level(gpio, 1);
        if (rx) vQueueDelete(rx);
        if (tx) vQueueDelete(tx);
        if (result) vQueueDelete(result);
        if (gate) vSemaphoreDelete(gate);
    }
};
bool Bus::Impl::tick(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *arg) {
    auto *p = static_cast<Impl *>(arg);
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&p->lock);
    const bool pull_low = p->engine.tick(esp_timer_get_time(), gpio_ll_get_level(&GPIO, p->gpio));
    gpio_ll_set_level(&GPIO, p->gpio, !pull_low);
    Frame frame;
    if (p->engine.take_received(frame)) {
        if (xQueueSendFromISR(p->rx, &frame, &wake) == pdTRUE) ++p->stats.received;
        else ++p->stats.rx_dropped;
    }
    const auto result = p->engine.take_result();
    if (result != TxResult::Pending) xQueueSendFromISR(p->result, &result, &wake);
    portEXIT_CRITICAL_ISR(&p->lock);
    return wake == pdTRUE;
}
Bus::Bus() : impl_(new Impl) {}
Bus::~Bus() = default;
esp_err_t Bus::start(int gpio) {
    auto &p = *impl_;
    if (p.timer) return ESP_ERR_INVALID_STATE;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return ESP_ERR_INVALID_ARG;
    p.gpio = static_cast<gpio_num_t>(gpio);
    p.rx = xQueueCreate(32, sizeof(Frame)); p.tx = xQueueCreate(64, sizeof(Impl::Work));
    p.result = xQueueCreate(1, sizeof(TxResult)); p.gate = xSemaphoreCreateMutex();
    if (!p.rx || !p.tx || !p.result || !p.gate) return ESP_ERR_NO_MEM;
    gpio_set_level(p.gpio, 1);
    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << gpio;
    pin.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    pin.pull_up_en = GPIO_PULLUP_DISABLE; pin.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t err = gpio_config(&pin);
    if (err != ESP_OK) return err;
    gptimer_config_t config{};
    config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    config.direction = GPTIMER_COUNT_UP;
    config.resolution_hz = 1000000;
    config.intr_priority = 3;
    // Share the level-3 interrupt between both CEC timers. BLE's controller
    // also requires a level-3 input on the startup core; allocating one
    // dedicated input per bus can exhaust the ESP32-S3 interrupt matrix.
    config.flags.intr_shared = true;
    if ((err = gptimer_new_timer(&config, &p.timer)) != ESP_OK) return err;
    gptimer_event_callbacks_t callbacks{}; callbacks.on_alarm = Impl::tick;
    if ((err = gptimer_register_event_callbacks(p.timer, &callbacks, &p)) != ESP_OK) return err;
    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = 50; alarm.reload_count = 0; alarm.flags.auto_reload_on_alarm = true;
    if ((err = gptimer_set_alarm_action(p.timer, &alarm)) != ESP_OK) return err;
    if ((err = gptimer_enable(p.timer)) != ESP_OK) return err;
    p.enabled = true;
    if ((err = gptimer_start(p.timer)) != ESP_OK) return err;
    p.started = true;
    if (xTaskCreate(Impl::run, "cec_tx", 3072, &p, 8, &p.task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
void Bus::set_address(uint8_t address) {
    portENTER_CRITICAL(&impl_->lock); impl_->engine.set_address(address); portEXIT_CRITICAL(&impl_->lock);
}
bool Bus::send(const Frame &frame, uint32_t timeout_ms) {
    const Impl::Work work{frame};
    return impl_->started && frame.size && frame.size <= 16 &&
           xQueueSend(impl_->tx, &work, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
bool Bus::send_and_wait(const Frame &frame, uint32_t timeout_ms, unsigned attempts) {
    if (!impl_->started || !frame.size || frame.size > 16 || !attempts) return false;
    const auto completed = xSemaphoreCreateBinary();
    if (!completed) return false;
    const Impl::Work work{frame, completed, attempts};
    const bool queued = xQueueSend(impl_->tx, &work, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    // Once admitted, keep the completion object alive until the worker signals.
    // The wire attempt itself has bounded arbitration/transmission deadlines.
    if (queued) xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return queued;
}
TxResult Bus::transmit(const Frame &frame, unsigned attempts) { return impl_->transmit(frame, attempts); }
bool Bus::receive(Frame &frame, uint32_t timeout_ms) {
    return impl_->started && xQueueReceive(impl_->rx, &frame, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
Statistics Bus::statistics() const {
    portENTER_CRITICAL(&impl_->lock); auto stats = impl_->stats; portEXIT_CRITICAL(&impl_->lock); return stats;
}
bool Bus::idle() const {
    auto &p = *impl_;
    if (!p.started) return true;
    portENTER_CRITICAL(&p.lock);
    const bool idle = !p.engine.busy();
    portEXIT_CRITICAL(&p.lock);
    return idle && uxQueueMessagesWaiting(p.tx) == 0;
}
}
