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
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include <cstdlib>

namespace cec {
constexpr unsigned nominal_tx_capacity = 8, maximum_tx_capacity = 9;
struct Bus::Impl {
    static void *operator new(size_t size) {
        void *memory = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) std::abort();
        return memory;
    }
    static void operator delete(void *memory) { heap_caps_free(memory); }
    struct Work {
        Frame frame;
        SemaphoreHandle_t completed = nullptr;
        unsigned attempts = 3;
        ExecutionFence fence{};
        TxResult *outcome = nullptr;
    };
    Engine engine;
    gpio_num_t gpio = GPIO_NUM_NC;
    gptimer_handle_t timer = nullptr;
    QueueHandle_t rx = nullptr, tx = nullptr, result = nullptr;
    SemaphoreHandle_t gate = nullptr, admission = nullptr, space_available = nullptr;
    TaskHandle_t task = nullptr;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    Statistics stats;
    bool enabled = false, started = false;
    unsigned tx_in_use = 0; // Includes the worker's current message, not just queued work.

    bool reserve_slots(size_t count, uint16_t overflow_mask) {
        portENTER_CRITICAL(&lock);
        auto proposed = tx_in_use;
        for (size_t i = 0; i < count; ++i) {
            const auto limit = (overflow_mask & (uint16_t{1} << i)) ? maximum_tx_capacity : nominal_tx_capacity;
            if (proposed >= limit) { portEXIT_CRITICAL(&lock); return false; }
            ++proposed;
        }
        tx_in_use = proposed;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    bool wait_reserve_slot(bool allow_overflow, uint32_t timeout_ms) {
        const auto start = uint64_t(esp_timer_get_time());
        const auto duration = uint64_t(timeout_ms) * 1000;
        for (;;) {
            if (reserve_slots(1, allow_overflow ? 1 : 0)) return true;
            if (allow_overflow || !timeout_ms) return false;
            const auto elapsed = uint64_t(esp_timer_get_time()) - start;
            if (elapsed >= duration) return false;
            const auto remaining_ms = static_cast<uint32_t>((duration - elapsed + 999) / 1000);
            const auto ticks = pdMS_TO_TICKS(remaining_ms);
            if (!ticks) return false;
            xSemaphoreTake(space_available, ticks);
        }
    }
    void free_slots(size_t count) {
        portENTER_CRITICAL(&lock);
        if (count > tx_in_use) std::abort();
        tx_in_use -= count;
        portEXIT_CRITICAL(&lock);
        xSemaphoreGive(space_available);
    }
    bool enqueue(const Work& work, uint32_t timeout_ms, bool allow_overflow) {
        if (!started || !work.frame.size || work.frame.size > 16 || !work.attempts) return false;
        if (xSemaphoreTake(admission, allow_overflow ? 0 : pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
        const bool reserved = wait_reserve_slot(allow_overflow, timeout_ms);
        const bool queued = reserved && xQueueSend(tx, &work, 0) == pdTRUE;
        if (reserved && !queued) free_slots(1);
        xSemaphoreGive(admission);
        return queued;
    }

    static bool IRAM_ATTR tick(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *arg);
    TxResult transmit(const Frame &frame, unsigned attempts, ExecutionFence fence = {}) {
        if (!started || !attempts || !frame.size || frame.size > 16) return TxResult::LineError;
        if (xSemaphoreTake(gate, pdMS_TO_TICKS(5000)) != pdTRUE) return TxResult::Timeout;
        TxResult status = TxResult::Timeout;
        for (unsigned attempt = 0; attempt < attempts; ++attempt) {
            if (fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us) { status = TxResult::Expired; break; }
            if (!fence_current(fence)) { status = TxResult::Cancelled; break; }
            xQueueReset(result);
            portENTER_CRITICAL(&lock);
            const bool begun = engine.begin(frame, esp_timer_get_time(), attempt > 0, fence);
            portEXIT_CRITICAL(&lock);
            if (!begun) {
                if (!fence_current(fence)) status = TxResult::Cancelled;
                else if (fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us) status = TxResult::Expired;
                break;
            }
            if (xQueueReceive(result, &status, pdMS_TO_TICKS(1600)) != pdTRUE) status = TxResult::Timeout;
            if (status == TxResult::Success || status == TxResult::Cancelled || status == TxResult::Expired) break;
        }
        portENTER_CRITICAL(&lock);
        if (status == TxResult::Success) ++stats.transmitted;
        else if (status != TxResult::Cancelled && status != TxResult::Expired &&
                 !(frame.size == 1 && status == TxResult::Nack)) ++stats.tx_errors;
        portEXIT_CRITICAL(&lock);
        xSemaphoreGive(gate);
        return status;
    }
    static void run(void *arg) {
        auto *p = static_cast<Impl *>(arg);
        Work work;
        for (;;) if (xQueueReceive(p->tx, &work, portMAX_DELAY) == pdTRUE) {
            const auto outcome = p->transmit(work.frame, work.attempts, work.fence);
            if (work.outcome) *work.outcome = outcome;
            p->free_slots(1);
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
        if (admission) vSemaphoreDelete(admission);
        if (space_available) vSemaphoreDelete(space_available);
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
    p.rx = xQueueCreate(32, sizeof(Frame)); p.tx = xQueueCreate(maximum_tx_capacity, sizeof(Impl::Work));
    p.result = xQueueCreate(1, sizeof(TxResult)); p.gate = xSemaphoreCreateMutex();
    p.admission = xSemaphoreCreateMutex(); p.space_available = xSemaphoreCreateBinary();
    if (!p.rx || !p.tx || !p.result || !p.gate || !p.admission || !p.space_available) return ESP_ERR_NO_MEM;
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
    return send_guarded(frame, {}, timeout_ms);
}
static bool valid_fence_storage(ExecutionFence fence) {
    const auto valid = [](const std::atomic<uint32_t> *counter) {
        return !counter ||
            ((reinterpret_cast<uintptr_t>(counter) % alignof(std::atomic<uint32_t>)) == 0 &&
             esp_ptr_internal(counter) && esp_ptr_byte_accessible(counter));
    };
    return valid(fence.generation) && valid(fence.secondary_generation);
}
bool Bus::send_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms, unsigned attempts, bool allow_overflow) {
    if (!valid_fence_storage(fence) || !fence_current(fence) || !attempts) return false;
    return impl_->enqueue({frame, nullptr, attempts, fence}, timeout_ms, allow_overflow);
}
bool Bus::send_and_wait(const Frame &frame, uint32_t timeout_ms, unsigned attempts) {
    return send_and_wait_guarded(frame, {}, timeout_ms, attempts);
}
bool Bus::send_and_wait_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms, unsigned attempts, TxResult* result, bool allow_overflow) {
    if (result) *result = TxResult::Pending;
    if (!impl_->started || !frame.size || frame.size > 16 || !attempts) return false;
    if (!valid_fence_storage(fence) || !fence_current(fence)) return false;
    const auto completed = xSemaphoreCreateBinary();
    if (!completed) return false;
    TxResult outcome = TxResult::Pending;
    const Impl::Work work{frame, completed, attempts, fence, &outcome};
    const bool queued = impl_->enqueue(work, timeout_ms, allow_overflow);
    // Once admitted, keep the completion object alive until the worker signals.
    // The wire attempt itself has bounded arbitration/transmission deadlines.
    if (queued) xSemaphoreTake(completed, portMAX_DELAY);
    if (result) *result = outcome;
    vSemaphoreDelete(completed);
    return queued && outcome != TxResult::Cancelled && outcome != TxResult::Expired;
}
TxResult Bus::transmit(const Frame &frame, unsigned attempts) { return transmit_guarded(frame, {}, attempts); }
TxResult Bus::transmit_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts) {
    if (!impl_->started || !frame.size || frame.size > 16 || !attempts || !valid_fence_storage(fence)) return TxResult::LineError;
    if (!fence_current(fence)) return TxResult::Cancelled;
    if (fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us) return TxResult::Expired;
    TxResult outcome = TxResult::Pending;
    send_and_wait_guarded(frame, fence, 5000, attempts, &outcome);
    return outcome == TxResult::Pending ? TxResult::Timeout : outcome;
}
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
    const bool idle = !p.engine.busy() && !p.tx_in_use;
    portEXIT_CRITICAL(&p.lock);
    return idle && uxQueueMessagesWaiting(p.tx) == 0;
}
}
