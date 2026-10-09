/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "device_settings.hpp"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "freertos/task.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Edge { uint64_t at; int level; };
struct Stopped {};
struct Restarted {};
uint64_t now = 0;
int level = 1, task_token;
bool in_isr = false, allocation_failure = false, handler_failure = false;
bool existing_service = false, inject_latched_edge = false, retry = false;
unsigned notifications = 0, schedules = 0, restarts = 0, yields = 0;
unsigned deleted_tasks = 0, reset_pins = 0, uninstalls = 0;
TaskFunction_t worker = nullptr;
gpio_isr_t isr = nullptr;
void *isr_arg = nullptr;
std::vector<Edge> edges;
size_t next_edge = 0;
std::vector<TickType_t> waits;

void interrupt(int new_level) {
    level = new_level;
    assert(isr);
    in_isr = true;
    isr(isr_arg);
    in_isr = false;
}
}

esp_err_t gpio_config(const gpio_config_t *config) {
    assert(config->pin_bit_mask == (1ULL << 5));
    assert(config->mode == GPIO_MODE_INPUT);
    assert(config->pull_up_en == GPIO_PULLUP_ENABLE);
    assert(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t gpio) {
    assert(gpio == 5 && !in_isr);
    int sampled = level;
    if (inject_latched_edge && now == 50) {
        inject_latched_edge = false;
        interrupt(0); // Between sampling the input and blocking the worker.
    }
    return sampled;
}
esp_err_t gpio_reset_pin(gpio_num_t gpio) { assert(gpio == 5); ++reset_pins; return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t gpio, int type) {
    assert(gpio == 5 && type == GPIO_INTR_ANYEDGE); return ESP_OK;
}
esp_err_t gpio_install_isr_service(int flags) {
    assert(flags == (ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_SHARED));
    return existing_service ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t gpio_uninstall_isr_service() { ++uninstalls; return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t handler, void *arg) {
    assert(gpio == 5 && arg == &task_token);
    if (handler_failure) return ESP_ERR_INVALID_STATE;
    isr = handler; isr_arg = arg; return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio) {
    assert(gpio == 5); isr = nullptr; return ESP_OK;
}
BaseType_t xTaskCreate(TaskFunction_t function, const char *, uint32_t stack, void *,
                       unsigned, TaskHandle_t *task) {
    assert(stack == 3072 && !in_isr);
    if (allocation_failure) return pdFALSE;
    worker = function; *task = &task_token; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task == &task_token); ++deleted_tasks; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *wake) {
    assert(in_isr && task == &task_token); ++notifications; *wake = pdTRUE;
}
void test_yield_from_isr() { assert(in_isr); ++yields; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
    assert(clear == pdTRUE && ticks > 0 && !in_isr);
    assert(waits.size() < 100); // Detect unintended periodic wakeups/spinning.
    waits.push_back(ticks);
    if (!notifications) {
        uint64_t deadline = ticks == portMAX_DELAY ? UINT64_MAX : now + uint64_t(ticks) * 10;
        if (next_edge < edges.size() && edges[next_edge].at <= deadline) {
            auto edge = edges[next_edge++]; now = edge.at; interrupt(edge.level);
        } else if (ticks == portMAX_DELAY) {
            throw Stopped{}; // The worker is indefinitely asleep, with no events.
        } else {
            now = deadline;
        }
    }
    auto count = notifications; notifications = 0; return count;
}
int64_t esp_timer_get_time() { return now * 1000; }
namespace device_settings {
esp_err_t schedule_factory_reset() {
    assert(!in_isr); ++schedules;
    return retry && schedules == 1 ? ESP_FAIL : ESP_OK;
}
}
[[noreturn]] void esp_restart() {
    assert(!in_isr); ++restarts; throw Restarted{};
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "boot-held") level = 0;
    else if (scenario == "hold") edges = {{100, 0}};
    else if (scenario == "short") edges = {{100, 0}, {500, 1}};
    else if (scenario == "cancel") edges = {{100, 0}, {10150, 1}};
    else if (scenario == "boot-release-hold") { level = 0; edges = {{100, 1}, {200, 0}}; }
    else if (scenario == "latched-edge") inject_latched_edge = true;
    else if (scenario == "retry") { retry = true; edges = {{100, 0}, {10200, 1}, {10300, 0}}; }
    else if (scenario == "allocation-failure") allocation_failure = true;
    else if (scenario == "shared-service-failure") { allocation_failure = true; existing_service = true; }
    else if (scenario == "handler-failure") handler_failure = true;
    else assert(scenario == "idle");

    assert(device_settings::start_reset_button(-1) == ESP_ERR_INVALID_ARG);
    auto result = device_settings::start_reset_button(5);
    if (allocation_failure || handler_failure) {
        assert(result == (allocation_failure ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE));
        assert(reset_pins == 1 && !isr);
        assert(deleted_tasks == unsigned(handler_failure));
        assert(uninstalls == unsigned(!existing_service));
    } else {
        assert(result == ESP_OK && worker && isr);
        assert(device_settings::start_reset_button(5) == ESP_ERR_INVALID_STATE);
        try { worker(nullptr); }
        catch (const Stopped &) {}
        catch (const Restarted &) {}
        const bool resets = scenario == "hold" || scenario == "boot-release-hold" ||
                            scenario == "latched-edge" || scenario == "retry";
        assert(restarts == unsigned(resets));
        assert(schedules == (retry ? 2U : unsigned(resets)));
        if (scenario == "hold") assert(now == 10150);
        if (scenario == "boot-release-hold") assert(now == 10250);
        if (scenario == "latched-edge") assert(now == 10100);
        if (scenario == "retry") assert(now == 20350);
        if (scenario == "idle" || scenario == "boot-held") {
            assert(waits.size() == 2 && waits[0] == 5 && waits[1] == portMAX_DELAY);
            assert(yields == 0 && now == 50);
        }
    }
    std::cout << "PASS FW_RESET interrupt adapter: " << scenario << '\n';
}
