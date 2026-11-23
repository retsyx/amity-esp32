/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The core SDK fakes behave as the firmware expects, on the scripted clock.
#include "core_fakes.hpp"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "seams.hpp"
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::string> fired;

void record_timer(void *name) { fired.push_back(static_cast<const char *>(name)); }

esp_timer_handle_t timer(const char *name) {
    esp_timer_create_args_t args{};
    args.callback = record_timer;
    args.arg = const_cast<char *>(name);
    args.name = name;
    esp_timer_handle_t handle = nullptr;
    CHECK(esp_timer_create(&args, &handle) == ESP_OK && handle);
    return handle;
}

void clock_timers() {
    CHECK(amity_fake::now_us() == 0 && esp_timer_get_time() == 0);
    auto late = timer("late"), early = timer("early");
    CHECK(esp_timer_start_once(late, 3000) == ESP_OK && esp_timer_start_once(early, 1000) == ESP_OK);
    CHECK(esp_timer_start_once(early, 5) == ESP_ERR_INVALID_STATE);
    amity_fake::advance_us(999);
    CHECK(fired.empty() && esp_timer_is_active(early));
    amity_fake::advance_us(5000);
    CHECK((fired == std::vector<std::string>{"early", "late"}) && esp_timer_get_time() == 5999);
    CHECK(!esp_timer_is_active(early) && esp_timer_stop(early) == ESP_ERR_INVALID_STATE);
    CHECK(esp_timer_delete(early) == ESP_OK && esp_timer_delete(late) == ESP_OK && amity_fake::live_timers() == 0);
    CHECK(esp_log_timestamp() == 5 && xTaskGetTickCount() == 0);
    amity_fake::advance_ms(10);
    CHECK(xTaskGetTickCount() == 1);
    // Only the two esp_timer_get_time calls read the clock.
    CHECK(amity_fake::clock_reads() == 2);
}

void periodic_timer() {
    auto tick = timer("tick");
    CHECK(esp_timer_start_periodic(tick, 100) == ESP_OK);
    amity_fake::advance_us(350);
    CHECK(fired.size() == 3);
    CHECK(esp_timer_delete(tick) == ESP_ERR_INVALID_STATE);
    CHECK(esp_timer_stop_blocking(tick, portMAX_DELAY) == ESP_OK && esp_timer_delete(tick) == ESP_OK);
    amity_fake::advance_us(1000);
    CHECK(fired.size() == 3);
}

void timer_failures() {
    esp_timer_create_args_t args{};
    args.callback = record_timer;
    esp_timer_handle_t handle = nullptr;
    amity_fake::fail_timer_creations(1);
    CHECK(esp_timer_create(&args, &handle) == ESP_ERR_NO_MEM && !handle);
    CHECK(esp_timer_create(&args, &handle) == ESP_OK);
    amity_fake::fail_timer_starts(1);
    CHECK(esp_timer_start_once(handle, 10) == ESP_FAIL && esp_timer_start_once(handle, 10) == ESP_OK);
    CHECK(esp_timer_restart(handle, 50) == ESP_OK);
    uint64_t expiry = 0;
    CHECK(esp_timer_get_expiry_time(handle, &expiry) == ESP_OK && expiry == 50);
    auto record = amity_fake::timers().back();
    CHECK(record.armed && record.deadline_us == 50 && record.starts == 1 && !record.deleted);
    CHECK(esp_timer_stop(handle) == ESP_OK && esp_timer_delete(handle) == ESP_OK);
    record = amity_fake::timers().back();
    CHECK(!record.armed && record.deleted && amity_fake::timers().size() == 1);
}

// A task loop that only ends when its delay hook throws.
void endless(void *counter) {
    for (;;) {
        ++*static_cast<int *>(counter);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void delay_leave() {
    int turns = 0;
    TaskHandle_t handle = nullptr;
    CHECK(xTaskCreate(endless, "endless", 4096, &turns, 3, &handle) == pdPASS && handle);
    CHECK(turns == 0 && amity_fake::tasks().at(0).state == amity_fake::Task::State::created);
    amity_fake::on_delay([](TickType_t ticks) {
        CHECK(ticks == 10);
        if (amity_fake::delays() == 3) throw amity_fake::LeaveTask{};
    });
    amity_fake::run_task(handle);
    CHECK(turns == 3 && amity_fake::delays() == 3 && amity_fake::now_us() == 200000);
    CHECK(amity_fake::tasks().at(0).state == amity_fake::Task::State::left);
    CHECK(pdMS_TO_TICKS(5) == 0 && pdMS_TO_TICKS(1000) == 100);
}

TaskHandle_t seen = nullptr;
void deletes_itself(void *argument) {
    seen = xTaskGetCurrentTaskHandle();
    CHECK(std::strcmp(pcTaskGetName(nullptr), "worker") == 0 && argument == &seen);
    vTaskDeleteWithCaps(nullptr);
    amity_test::fail(__FILE__, __LINE__, "vTaskDeleteWithCaps returned");
}

void task_records() {
    amity_fake::fail_task_creations(1);
    CHECK(xTaskCreateWithCaps(deletes_itself, "worker", 16384, &seen, 2, nullptr, MALLOC_CAP_SPIRAM) ==
          errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY);
    CHECK(amity_fake::tasks().empty());
    CHECK(xTaskCreateWithCaps(deletes_itself, "worker", 16384, &seen, 2, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
          pdPASS);
    const auto task = amity_fake::tasks().at(0);
    CHECK(task.name == "worker" && task.stack_bytes == 16384 && task.priority == 2 && task.with_caps);
    CHECK(task.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) && task.core == tskNO_AFFINITY);
    amity_fake::run_task(task.handle);
    CHECK(seen == task.handle && amity_fake::tasks().at(0).state == amity_fake::Task::State::deleted);
    CHECK(std::strcmp(pcTaskGetName(nullptr), "main") == 0 && xTaskGetCurrentTaskHandle() != task.handle);
}

void returns(void *) {}
void task_return() {
    TaskHandle_t handle = nullptr;
    CHECK(xTaskCreate(returns, "returns", 2048, nullptr, 1, &handle) == pdPASS);
    amity_fake::run_task(handle);
}

void deletes_wrongly(void *) { vTaskDelete(nullptr); }
void task_caps_mismatch() {
    TaskHandle_t handle = nullptr;
    CHECK(xTaskCreateWithCaps(deletes_wrongly, "caps", 2048, nullptr, 1, &handle, MALLOC_CAP_SPIRAM) == pdPASS);
    amity_fake::run_task(handle);
}

struct Item {
    int value;
    char tag;
};

void static_queue() {
    static Item storage[3];
    static StaticQueue_t control;
    QueueHandle_t queue = xQueueCreateStatic(3, sizeof(Item), reinterpret_cast<uint8_t *>(storage), &control);
    CHECK(queue == reinterpret_cast<QueueHandle_t>(&control));
    const auto record = amity_fake::queues().at(0);
    CHECK(record.is_static && record.length == 3 && record.item_size == sizeof(Item));
    for (int i = 0; i < 3; ++i) {
        const Item item{i, char('a' + i)};
        CHECK(xQueueSend(queue, &item, 0) == pdTRUE);
    }
    CHECK(storage[0].value == 0 && storage[2].tag == 'c');
    const Item extra{9, 'z'};
    CHECK(xQueueSend(queue, &extra, 0) == errQUEUE_FULL && uxQueueMessagesWaiting(queue) == 3);
    Item out{};
    CHECK(xQueueReceive(queue, &out, 0) == pdTRUE && out.value == 0);
    CHECK(xQueueSend(queue, &extra, 0) == pdTRUE && storage[0].value == 9);
    const Item urgent{7, 'u'};
    CHECK(xQueueReceive(queue, &out, 0) == pdTRUE && out.value == 1);
    CHECK(xQueueSendToFront(queue, &urgent, 0) == pdTRUE);
    std::vector<int> order;
    while (xQueueReceive(queue, &out, 0) == pdTRUE) order.push_back(out.value);
    CHECK((order == std::vector<int>{7, 2, 9}) && uxQueueSpacesAvailable(queue) == 3);
    CHECK(xQueueSend(queue, &extra, 0) == pdTRUE && xQueueReset(queue) == pdPASS && uxQueueMessagesWaiting(queue) == 0);
    vQueueDelete(queue);
    CHECK(amity_fake::queues().at(0).deleted);
}

void queue_timeouts() {
    QueueHandle_t queue = xQueueCreate(1, sizeof(int));
    int value = 5;
    CHECK(xQueueReceive(queue, &value, pdMS_TO_TICKS(50)) == errQUEUE_EMPTY && amity_fake::now_us() == 50000);
    CHECK(xQueueSend(queue, &value, 0) == pdTRUE && xQueueSend(queue, &value, pdMS_TO_TICKS(20)) == errQUEUE_FULL);
    CHECK(amity_fake::now_us() == 70000);
    amity_fake::fail_queue_creations(1);
    CHECK(xQueueCreate(4, 4) == nullptr && xSemaphoreCreateMutex() != nullptr);
    vQueueDelete(queue);
}

void semaphores() {
    SemaphoreHandle_t binary = xSemaphoreCreateBinary();
    CHECK(xSemaphoreTake(binary, 0) == pdFALSE && xSemaphoreGive(binary) == pdTRUE && xSemaphoreGive(binary) == pdFALSE);
    CHECK(uxSemaphoreGetCount(binary) == 1 && xSemaphoreTake(binary, portMAX_DELAY) == pdTRUE);
    SemaphoreHandle_t counting = xSemaphoreCreateCounting(2, 1);
    CHECK(xSemaphoreGive(counting) == pdTRUE && xSemaphoreGive(counting) == pdFALSE && uxSemaphoreGetCount(counting) == 2);
    SemaphoreHandle_t recursive = xSemaphoreCreateRecursiveMutex();
    CHECK(xSemaphoreTakeRecursive(recursive, 0) == pdTRUE && xSemaphoreTakeRecursive(recursive, 0) == pdTRUE);
    CHECK(xSemaphoreGiveRecursive(recursive) == pdTRUE && uxSemaphoreGetCount(recursive) == 0);
    CHECK(xSemaphoreGiveRecursive(recursive) == pdTRUE && uxSemaphoreGetCount(recursive) == 1);
    static StaticSemaphore_t control;
    SemaphoreHandle_t fixed = xSemaphoreCreateMutexStatic(&control);
    CHECK(fixed == reinterpret_cast<SemaphoreHandle_t>(&control) && amity_fake::queues().back().is_static);
    vSemaphoreDelete(binary);
    vSemaphoreDelete(counting);
    vSemaphoreDelete(recursive);
    vSemaphoreDelete(fixed);
}

void mutex_ownership() {
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    CHECK(xSemaphoreGive(mutex) == pdFALSE && xSemaphoreTake(mutex, 0) == pdTRUE);
    CHECK(xSemaphoreGetMutexHolder(mutex) == xTaskGetCurrentTaskHandle());
    bool given = true;
    std::thread other([&] { given = xSemaphoreGive(mutex) == pdTRUE; });
    other.join();
    CHECK(!given && xSemaphoreGive(mutex) == pdTRUE && xSemaphoreGetMutexHolder(mutex) == nullptr);
}

SemaphoreHandle_t ready;
void give_ready(void *) { xSemaphoreGive(ready); }

void timer_wakes_wait() {
    ready = xSemaphoreCreateBinary();
    esp_timer_create_args_t args{};
    args.callback = give_ready;
    esp_timer_handle_t handle = nullptr;
    CHECK(esp_timer_create(&args, &handle) == ESP_OK && esp_timer_start_once(handle, 30000) == ESP_OK);
    CHECK(xSemaphoreTake(ready, pdMS_TO_TICKS(1000)) == pdTRUE && amity_fake::now_us() == 30000);
    CHECK(xSemaphoreTake(ready, pdMS_TO_TICKS(10)) == pdFALSE && amity_fake::now_us() == 40000);
}

void blocking_threads() {
    amity_fake::waits(amity_fake::Waits::block);
    QueueHandle_t queue = xQueueCreate(1, sizeof(int));
    std::thread producer([&] {
        const int value = 42;
        CHECK(xQueueSend(queue, &value, portMAX_DELAY) == pdTRUE);
    });
    int value = 0;
    CHECK(xQueueReceive(queue, &value, portMAX_DELAY) == pdTRUE && value == 42);
    producer.join();
    std::atomic<bool> done{false};
    std::thread clock([&] {
        while (!done.load()) amity_fake::advance_ms(100);
    });
    CHECK(xQueueReceive(queue, &value, pdMS_TO_TICKS(500)) == errQUEUE_EMPTY);
    done.store(true);
    clock.join();
    CHECK(amity_fake::now_us() >= 500000);
}

void endless_wait() {
    SemaphoreHandle_t never = xSemaphoreCreateBinary();
    CHECK(never != nullptr);
    xSemaphoreTake(never, portMAX_DELAY);
}

void notifications() {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    CHECK(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) == 0 && amity_fake::now_us() == 10000);
    CHECK(xTaskNotifyGive(self) == pdPASS && xTaskNotifyGive(self) == pdPASS);
    CHECK(ulTaskNotifyTake(pdFALSE, 0) == 2 && ulTaskNotifyTake(pdTRUE, 0) == 1 && ulTaskNotifyTake(pdTRUE, 0) == 0);
}

std::string sunk;
int sink(const char *format, va_list arguments) {
    char line[256];
    const int size = std::vsnprintf(line, sizeof(line), format, arguments);
    sunk += line;
    return size;
}

void logs() {
    amity_fake::advance_ms(1234);
    std::vector<std::string> seen_by_hook;
    amity_fake::on_log([&](const amity_fake::LogLine &line) { seen_by_hook.push_back(line.message); });
    CHECK(esp_log_set_vprintf(sink) == vprintf);
    ESP_LOGI("group", "peer %u joined %s", 3u, "living-room");
    ESP_LOGW("group", "plain");
    ESP_LOGD("group", "hidden %d", 1);
    const auto lines = amity_fake::log_lines();
    CHECK(lines.size() == 2 && lines[0].level == ESP_LOG_INFO && lines[0].tag == "group");
    CHECK(lines[0].message == "peer 3 joined living-room" && lines[1].level == ESP_LOG_WARN);
    CHECK(amity_fake::logged("joined") && !amity_fake::logged("hidden"));
    CHECK((seen_by_hook == std::vector<std::string>{"peer 3 joined living-room", "plain"}));
    CHECK(sunk == "I (1234) group: peer 3 joined living-room\nW (1234) group: plain\n");
}

esp_err_t failing() { return ESP_ERR_TIMEOUT; }
void restart_and_checks() {
    bool restarted = false;
    try {
        esp_restart();
    } catch (const amity_fake::Restart &) {
        restarted = true;
    }
    CHECK(restarted);
    bool aborted = false;
    try {
        ESP_ERROR_CHECK(failing());
    } catch (const amity_fake::ErrorCheckFailed &failure) {
        aborted = failure.error == ESP_ERR_TIMEOUT && failure.expression == "failing()";
    }
    CHECK(aborted);
    CHECK(std::strcmp(esp_err_to_name(ESP_ERR_INVALID_STATE), "ESP_ERR_INVALID_STATE") == 0);
    CHECK(std::strcmp(esp_err_to_name(0x1102), "UNKNOWN ERROR") == 0);
    char name[8];
    CHECK(std::strcmp(esp_err_to_name_r(ESP_FAIL, name, sizeof(name)), "ESP_FAI") == 0);
    CHECK(esp_register_shutdown_handler(endless_wait) == ESP_OK);
    CHECK(esp_register_shutdown_handler(endless_wait) == ESP_ERR_INVALID_STATE);
    CHECK(amity_fake::shutdown_handlers().size() == 1);
}

std::vector<uint32_t> failed_caps;
void failed_allocation(size_t, uint32_t caps, const char *) { failed_caps.push_back(caps); }

void heap() {
    void *psram = heap_caps_malloc(64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    CHECK(amity_fake::allocation_caps(psram) == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) && amity_fake::live_allocations() == 1);
    void *zeroed = heap_caps_calloc(4, 4, MALLOC_CAP_INTERNAL);
    CHECK(zeroed && static_cast<uint8_t *>(zeroed)[15] == 0);
    CHECK(heap_caps_register_failed_alloc_callback(failed_allocation) == ESP_OK);
    amity_fake::fail_allocations(1);
    CHECK(heap_caps_malloc(8, MALLOC_CAP_DMA) == nullptr && (failed_caps == std::vector<uint32_t>{MALLOC_CAP_DMA}));
    heap_caps_free(psram);
    heap_caps_free(zeroed);
    CHECK(amity_fake::live_allocations() == 0 && amity_fake::allocation_caps(psram) == 0);
    heap_caps_malloc_extmem_enable(512);
    CHECK(amity_fake::extmem_limit() == 512);
}

void strict_seam() { (void)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

void seam_behaviour() {
    amity_seam::install<&heap_caps_get_free_size>([](uint32_t caps) { return caps == MALLOC_CAP_SPIRAM ? 4096 : 1024; });
    amity_seam::install<&esp_get_free_heap_size>([] { return uint32_t(5120); });
    CHECK(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 4096 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) == 1024);
    CHECK(esp_get_free_heap_size() == 5120);
    CHECK((amity_seam::calls() ==
           std::vector<std::string>{"heap_caps_get_free_size", "heap_caps_get_free_size", "esp_get_free_heap_size"}));
    CHECK(amity_seam::count("heap_caps_get_free_size") == 2);
    amity_seam::clear_calls();
    amity_seam::uninstall<&esp_get_free_heap_size>();
    CHECK(amity_seam::calls().empty() && !amity_seam::behaviour<&esp_get_free_heap_size>());
}

void critical_sections() {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    CHECK(mux.owner == 0xB33FFFFF && mux.count == 0);
    portENTER_CRITICAL(&mux);
    portENTER_CRITICAL_SAFE(&mux);
    CHECK(mux.count == 2);
    portEXIT_CRITICAL_SAFE(&mux);
    portEXIT_CRITICAL(&mux);
    CHECK(mux.count == 0 && xPortInIsrContext() == pdFALSE);
    amity_fake::in_isr(true);
    CHECK(xPortInIsrContext() == pdTRUE);
}

void app_description() {
    CHECK(sizeof(esp_app_desc_t) == 256 && sizeof(esp_image_header_t) == 24 && sizeof(esp_image_segment_header_t) == 8);
    const esp_app_desc_t *running = esp_app_get_description();
    CHECK(running->magic_word == ESP_APP_DESC_MAGIC_WORD && std::strcmp(running->project_name, "amity") == 0);
    amity_fake::app_description().app_elf_sha256[0] = 0xab;
    amity_fake::app_description().app_elf_sha256[31] = 0x01;
    char digest[65];
    CHECK(esp_app_get_elf_sha256(digest, sizeof(digest)) == 65 && std::strlen(digest) == 64);
    CHECK(std::strncmp(digest, "ab00", 4) == 0 && std::strcmp(digest + 62, "01") == 0);
    char prefix[5];
    CHECK(esp_app_get_elf_sha256(prefix, sizeof(prefix)) == 5 && std::strcmp(prefix, "ab00") == 0);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"clock-timers", clock_timers},
                            {"periodic-timer", periodic_timer},
                            {"timer-failures", timer_failures},
                            {"delay-leave", delay_leave},
                            {"task-records", task_records},
                            {"task-return", task_return},
                            {"task-caps-mismatch", task_caps_mismatch},
                            {"static-queue", static_queue},
                            {"queue-timeouts", queue_timeouts},
                            {"semaphores", semaphores},
                            {"mutex-ownership", mutex_ownership},
                            {"timer-wakes-wait", timer_wakes_wait},
                            {"blocking-threads", blocking_threads},
                            {"endless-wait", endless_wait},
                            {"notifications", notifications},
                            {"logs", logs},
                            {"restart-and-checks", restart_and_checks},
                            {"heap", heap},
                            {"strict-seam", strict_seam},
                            {"seam-behaviour", seam_behaviour},
                            {"critical-sections", critical_sections},
                            {"app-description", app_description}});
}
