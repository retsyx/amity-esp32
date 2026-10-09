/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "device_settings.hpp"
#include "sdkconfig.h"

#if CONFIG_AMITY_FW_RESET_BUTTON
#include "reset_button.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char tag[] = "fw_reset";
gpio_num_t button_gpio;
bool started = false;

void IRAM_ATTR button_isr(void *arg) {
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(arg), &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

void button_task(void *) {
    device_settings::ResetButton button;
    while (true) {
        const auto now_ms = esp_timer_get_time() / 1000;
        if (button.update(gpio_get_level(button_gpio) == 0, now_ms)) {
            auto err = device_settings::schedule_factory_reset();
            if (err == ESP_OK) {
                ESP_LOGW(tag, "FW_RESET held: factory reset scheduled; restarting");
                // Erasure runs at the next boot, before networking starts.
                esp_restart();
            }
            ESP_LOGE(tag, "Could not schedule factory reset: %s; release and hold to retry",
                     esp_err_to_name(err));
        }
        const auto wait_ms = button.next_check_ms(now_ms);
        const auto ticks = wait_ms == device_settings::ResetButton::no_deadline
            ? portMAX_DELAY
            : pdMS_TO_TICKS(wait_ms + portTICK_PERIOD_MS - 1);
        // Notifications stay latched if an edge arrives before this wait.
        // A timeout checks debounce/hold once; there is no periodic polling.
        ulTaskNotifyTake(pdTRUE, ticks);
    }
}
}
#endif

namespace device_settings {
esp_err_t start_reset_button(int gpio) {
#if CONFIG_AMITY_FW_RESET_BUTTON
    if (started) return ESP_ERR_INVALID_STATE;
    if (!GPIO_IS_VALID_GPIO(gpio)) return ESP_ERR_INVALID_ARG;
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << gpio;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    auto err = gpio_config(&config);
    if (err != ESP_OK) return err;
    button_gpio = static_cast<gpio_num_t>(gpio);

    // Share an interrupt input where possible, leaving resources for CEC/BLE.
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_SHARED);
    const bool owns_service = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        gpio_reset_pin(button_gpio);
        return err;
    }
    TaskHandle_t task = nullptr;
    auto cleanup = [&](esp_err_t result) {
        gpio_isr_handler_remove(button_gpio);
        if (task) vTaskDelete(task);
        gpio_reset_pin(button_gpio);
        if (owns_service) gpio_uninstall_isr_service();
        return result;
    };
    if (xTaskCreate(button_task, "fw_reset", 3072, nullptr, 2, &task) != pdPASS)
        return cleanup(ESP_ERR_NO_MEM);
    err = gpio_set_intr_type(button_gpio, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) return cleanup(err);
    err = gpio_isr_handler_add(button_gpio, button_isr, task);
    if (err != ESP_OK) return cleanup(err);
    started = true;
    ESP_LOGI(tag, "GPIO%d enabled: release, then hold FW_RESET for 10 seconds", gpio);
    return ESP_OK;
#else
    (void)gpio;
    return ESP_OK;
#endif
}
} // namespace device_settings
