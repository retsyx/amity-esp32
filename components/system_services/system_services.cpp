/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "system_services.hpp"
#include "web.hpp"
#include "firmware_version.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
namespace system_services {
namespace {
EXT_RAM_BSS_ATTR std::array<char, 24576> ring{};
size_t cursor = 0, used = 0;
SemaphoreHandle_t lock=nullptr;
char boot_identifier[17]{};
std::atomic<bool> ready{false};
vprintf_like_t original = nullptr;
int capture(const char *fmt, va_list args) {
    char line[384];
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (length > 0) {
        size_t n = std::min(static_cast<size_t>(length), sizeof(line)-1);
        xSemaphoreTake(lock,portMAX_DELAY);
        for (size_t i=0; i<n; ++i) { ring[cursor] = line[i]; cursor=(cursor+1)%ring.size(); }
        used = std::min(used+n, ring.size());
        xSemaphoreGive(lock);
    }
    return original ? original(fmt, args) : length;
}
void restart_task(void *) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }
}
void start_log_capture() {
    snprintf(boot_identifier,sizeof(boot_identifier),"%08lx%08lx",
             static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
    lock=xSemaphoreCreateMutex();if(lock)original = esp_log_set_vprintf(capture);
}
const char *boot_id() { return boot_identifier; }
bool firmware_ready() { return ready.load(); }
void log_memory_if_due() {
    // Called by one background task. Wait for all services before the baseline.
    static int64_t next_log_us = 0;
    if (!firmware_ready()) return;
    const int64_t now = esp_timer_get_time();
    if (now < next_log_us) return;
    constexpr int64_t interval_us = 10LL * 60 * 1000000;
    next_log_us = now + interval_us;

    multi_heap_info_t internal{}, psram{};
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI("memory", "uptime=%" PRId64 "s bytes: internal free=%zu min=%zu largest=%zu; "
             "psram free=%zu min=%zu largest=%zu",
             now / 1000000, internal.total_free_bytes, internal.minimum_free_bytes,
             internal.largest_free_block, psram.total_free_bytes, psram.minimum_free_bytes,
             psram.largest_free_block);
}
size_t log_size() {
    if (!lock) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    const auto size = used;
    xSemaphoreGive(lock);
    return size;
}
std::string logs() {
    std::string result(ring.size(), '\0');
    if(!lock)return {};
    xSemaphoreTake(lock,portMAX_DELAY);
    size_t n=used, start=(cursor+ring.size()-n)%ring.size();
    for (size_t i=0;i<n;++i) result[i]=ring[(start+i)%ring.size()];
    xSemaphoreGive(lock);
    result.resize(n);
    return result;
}
void reboot_later() { xTaskCreate(restart_task,"restart",2048,nullptr,3,nullptr); }
esp_err_t confirm_firmware() {
    esp_ota_img_states_t state;
    const auto *running=esp_ota_get_running_partition();
    esp_err_t result=ESP_OK;
    if (running && esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY)
        result=esp_ota_mark_app_valid_cancel_rollback();
    if(result==ESP_OK)ready.store(true);
    return result;
}
esp_err_t upload_firmware(httpd_req_t *req) {
    const auto *target=esp_ota_get_next_update_partition(nullptr);
    constexpr size_t header_size=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    if (!target || req->content_len<header_size || req->content_len>target->size)
        return web::error(req,"Invalid firmware size");
    web::BodyReader reader(req,120ULL*1000000);
    if(!reader.valid())return web::error(req,"Cannot receive firmware","503 Service Unavailable");
    // Validate the complete application header before erasing the inactive slot.
    std::array<uint8_t,2048> buf{};
    size_t read=0;
    while(read<header_size) {
        int n=reader.receive(reinterpret_cast<char *>(buf.data()+read),header_size-read);
        if(n<=0) return web::error(req,"Incomplete firmware header","408 Request Timeout");
        read+=n;
    }
    esp_image_header_t image{};
    esp_app_desc_t app{};
    memcpy(&image,buf.data(),sizeof(image));
    memcpy(&app,buf.data()+sizeof(image)+sizeof(esp_image_segment_header_t),sizeof(app));
    const esp_app_desc_t *running=esp_app_get_description();
    if(image.magic!=ESP_IMAGE_HEADER_MAGIC || image.chip_id!=CONFIG_IDF_FIRMWARE_CHIP_ID ||
       app.magic_word!=ESP_APP_DESC_MAGIC_WORD || memcmp(app.project_name,running->project_name,sizeof(app.project_name))!=0)
        return web::error(req,"Firmware is not for this Amity target");
    if(!memchr(app.version,'\0',sizeof(app.version)))
        return web::error(req,"Invalid firmware version field");
    const auto comparison=compare_firmware_versions(app.version,running->version);
    if(!comparison||*comparison<0) {
        // Bind approval to the running version, so a stale browser cannot silently approve a downgrade.
        char confirmed[sizeof(running->version)]{};
        if(httpd_req_get_hdr_value_str(req,"X-Amity-Confirm-Version",confirmed,sizeof(confirmed))!=ESP_OK||
           strcmp(confirmed,running->version)!=0) {
            return web::error(req,comparison?
                "Firmware downgrade requires explicit confirmation. Reload the page and select the file again.":
                "Firmware versions cannot be compared. Reload the page and select the file again to confirm.","409 Conflict");
        }
    }
    esp_ota_handle_t handle;
    esp_err_t e=esp_ota_begin(target,req->content_len,&handle);
    if(e!=ESP_OK) return web::error(req,esp_err_to_name(e),"500 Internal Server Error");
    e=esp_ota_write(handle,buf.data(),read);
    while(e==ESP_OK && read<req->content_len) {
        int n=reader.receive(reinterpret_cast<char *>(buf.data()),std::min(buf.size(),req->content_len-read));
        if(n<=0) { e=ESP_ERR_TIMEOUT; break; }
        e=esp_ota_write(handle,buf.data(),n);
        read+=n;
    }
    if(e!=ESP_OK) { esp_ota_abort(handle); return web::error(req,"Firmware upload failed; current firmware retained"); }
    e=esp_ota_end(handle);
    if(e==ESP_OK) e=esp_ota_set_boot_partition(target);
    if(e!=ESP_OK) return web::error(req,"Firmware validation failed");
    httpd_resp_set_type(req,"application/json");
    e=httpd_resp_sendstr(req,"{\"rebooting\":true}");
    reboot_later();
    return e;
}
}
