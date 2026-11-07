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
#include "bootloader_random.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "firmware_update.hpp"
#include "group_firmware_update.hpp"
#include "update_bundle.hpp"
#include "psa/crypto.h"
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
#include <optional>
namespace system_services {
namespace {
EXT_RAM_BSS_ATTR std::array<char, 24576> ring{};
size_t cursor = 0, used = 0;
SemaphoreHandle_t lock=nullptr;
char boot_identifier[33]{};
std::atomic<bool> ready{false};
vprintf_like_t original = nullptr;
struct AllocationFailure {
    uint64_t at_ms=0;
    size_t bytes=0;
    uint32_t caps=0;
    size_t matched_free=0,dma_free=0,internal_free=0;
    char function[32]{},task[16]{};
};
// The allocator calls its failure hook after releasing the heap lock. Capture
// fixed-size observations only: logging here could allocate or recurse through
// an application/log mutex held by the failing caller. The existing background
// poll emits these event records later, outside this guard and allocator call.
std::array<AllocationFailure,8> allocation_failures{};
size_t allocation_failure_read=0,allocation_failure_count=0;
std::atomic_flag allocation_failure_guard=ATOMIC_FLAG_INIT;
std::atomic<uint32_t> allocation_failure_suppressed{0};
void capture_allocation_failure(size_t bytes,uint32_t caps,const char* function){
    if(allocation_failure_guard.test_and_set(std::memory_order_acquire)){
        allocation_failure_suppressed.fetch_add(1,std::memory_order_relaxed);return;
    }
    if(allocation_failure_count==allocation_failures.size()){
        allocation_failure_suppressed.fetch_add(1,std::memory_order_relaxed);
        allocation_failure_guard.clear(std::memory_order_release);return;
    }
    auto& failure=allocation_failures[(allocation_failure_read+allocation_failure_count)%allocation_failures.size()];
    failure={};failure.at_ms=static_cast<uint64_t>(esp_timer_get_time()/1000);failure.bytes=bytes;failure.caps=caps;
    auto copy=[](char* destination,size_t capacity,const char* source){
        size_t length=0;if(source)while(length+1<capacity&&source[length]){destination[length]=source[length];++length;}
        destination[length]='\0';
    };
    copy(failure.function,sizeof(failure.function),function);
    copy(failure.task,sizeof(failure.task),xPortInIsrContext()?"ISR":pcTaskGetName(nullptr));
    // Read maintained counters only. Full heap statistics walk every block
    // with interrupts masked, which can interrupt an in-flight CEC frame.
    failure.matched_free=heap_caps_get_free_size(caps);
    failure.dma_free=heap_caps_get_free_size(MALLOC_CAP_DMA);
    failure.internal_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    ++allocation_failure_count;
    allocation_failure_guard.clear(std::memory_order_release);
}
void log_allocation_failures(){
    for(size_t emitted=0;emitted<allocation_failures.size();++emitted){
        if(allocation_failure_guard.test_and_set(std::memory_order_acquire))return;
        if(!allocation_failure_count){allocation_failure_guard.clear(std::memory_order_release);break;}
        const auto failure=allocation_failures[allocation_failure_read];
        allocation_failure_read=(allocation_failure_read+1)%allocation_failures.size();--allocation_failure_count;
        allocation_failure_guard.clear(std::memory_order_release);
        ESP_LOGE("memory","allocation_failed at_ms=%" PRIu64 " bytes=%zu caps=0x%08" PRIx32 " fn=%s task=%s matched_free=%zu dma_free=%zu internal_free=%zu",
                 failure.at_ms,failure.bytes,failure.caps,failure.function,failure.task,
                 failure.matched_free,failure.dma_free,failure.internal_free);
    }
    const auto suppressed=allocation_failure_suppressed.exchange(0,std::memory_order_relaxed);
    if(suppressed)ESP_LOGE("memory","allocation_failed suppressed=%" PRIu32 " reason=concurrent_or_full",suppressed);
}
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
void restart_task(void *delay) { vTaskDelay(pdMS_TO_TICKS(reinterpret_cast<uintptr_t>(delay))); esp_restart(); }
}
void start_log_capture() {
    uint32_t entropy[4];
    bootloader_random_enable();
    esp_fill_random(entropy,sizeof(entropy));
    bootloader_random_disable();
    snprintf(boot_identifier,sizeof(boot_identifier),"%08lx%08lx%08lx%08lx",
             static_cast<unsigned long>(entropy[0]),static_cast<unsigned long>(entropy[1]),
             static_cast<unsigned long>(entropy[2]),static_cast<unsigned long>(entropy[3]));
    lock=xSemaphoreCreateMutex();if(lock)original = esp_log_set_vprintf(capture);
    if(heap_caps_register_failed_alloc_callback(capture_allocation_failure)!=ESP_OK)ESP_LOGW("memory","Allocation failure diagnostics are unavailable");
}
const char *boot_id() { return boot_identifier; }
bool firmware_ready() { return ready.load(); }
void log_memory_if_due() {
    log_allocation_failures();
    // Called by one background task. Wait for all services before the baseline.
    static int64_t next_log_us = 0;
    if (!firmware_ready()) return;
    const int64_t now = esp_timer_get_time();
    if (now < next_log_us) return;
    constexpr int64_t interval_us = 10LL * 60 * 1000000;
    next_log_us = now + interval_us;

    // Keep diagnostics independent of heap fragmentation and the number of
    // allocations: heap walks hold interrupt-masking locks across the pool.
    constexpr uint32_t internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    constexpr uint32_t psram=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    ESP_LOGI("memory", "uptime=%" PRId64 "s bytes: internal free=%zu min=%zu; psram free=%zu min=%zu",
             now / 1000000, heap_caps_get_free_size(internal),heap_caps_get_minimum_free_size(internal),
             heap_caps_get_free_size(psram),heap_caps_get_minimum_free_size(psram));
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
void reboot_later(uint32_t delay_ms) { xTaskCreate(restart_task,"restart",2048,reinterpret_cast<void*>(uintptr_t(delay_ms)),3,nullptr); }
void reboot_later() { reboot_later(1500); }
esp_err_t confirm_firmware() {
    esp_ota_img_states_t state;
    const auto *running=esp_ota_get_running_partition();
    esp_err_t result=ESP_OK;
    if (running && esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY)
        result=esp_ota_mark_app_valid_cancel_rollback();
    if(result==ESP_OK){ready.store(true);updates::local_boot_healthy();}
    return result;
}
esp_err_t upload_firmware(httpd_req_t *req) {
    constexpr size_t header_size=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    const auto *target=esp_ota_get_next_update_partition(nullptr);
    constexpr size_t metadata_cap=updates::update_bundle_header_bytes+8192+64;
    if(!target||req->content_len<header_size||uint64_t(req->content_len)>uint64_t(target->size)+metadata_cap)
        return web::error(req,"Invalid firmware size");
    web::BodyReader reader(req,120ULL*1000000);
    if(!reader.valid())return web::error(req,"Cannot receive firmware","503 Service Unavailable");
    std::optional<web::BodyReader> metadata_reader;
    metadata_reader.emplace(req,30ULL*1000000);
    if(!metadata_reader->valid())return web::error(req,"Cannot receive firmware metadata","503 Service Unavailable");
    auto read_exact=[&](char* destination,size_t count){
        size_t received=0;
        while(received<count){const int n=metadata_reader->receive(destination+received,count-received);if(n<=0)return false;received+=n;}
        return true;
    };
    std::array<char,updates::update_bundle_header_bytes> prefix{};
    if(!read_exact(prefix.data(),prefix.size()))return web::error(req,"Incomplete firmware header");
    updates::UpdateBundleHeader bundle;std::string error;
    if(!updates::parse_update_bundle_header(std::string_view(prefix.data(),prefix.size()),req->content_len,bundle,error))return web::error(req,error.c_str());
    std::string manifest(bundle.manifest_bytes,'\0'),signature(bundle.signature_bytes,'\0');
    if(!read_exact(manifest.data(),manifest.size())||!read_exact(signature.data(),signature.size()))return web::error(req,"Incomplete update file metadata");
    std::string signature_hex;signature_hex.reserve(signature.size()*2);constexpr char digits[]="0123456789abcdef";
    for(unsigned char byte:signature){signature_hex+=digits[byte>>4];signature_hex+=digits[byte&15];}
    updates::ReleaseManifest release;
    if(!updates::parse_release_manifest(manifest,release,error)||release.image_bytes!=bundle.image_bytes)return web::error(req,"Firmware image size does not match the release");
    std::array<char,2048> buf{};
    const auto first=std::min<uint64_t>(buf.size(),bundle.image_bytes);
    if(first<header_size||!read_exact(buf.data(),first))return web::error(req,"Incomplete firmware header");
    metadata_reader.reset();
    esp_app_desc_t app{};
    memcpy(&app,buf.data()+sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t),sizeof(app));
    const auto *running=esp_app_get_description();
    if(!memchr(app.version,'\0',sizeof(app.version)))return web::error(req,"Invalid firmware version field");
    const auto comparison=compare_firmware_versions(app.version,running->version);
    if(!comparison||*comparison<0){
        char confirmed[sizeof(running->version)]{};
        if(httpd_req_get_hdr_value_str(req,"X-Amity-Confirm-Version",confirmed,sizeof(confirmed))!=ESP_OK||strcmp(confirmed,running->version)!=0)
            return web::error(req,"Firmware downgrade requires explicit confirmation. Reload the page and select the file again.","409 Conflict");
    }
    if(updates::begin_system_upload(release,error)!=ESP_OK)return web::error(req,error.c_str(),"409 Conflict");
    auto fail=[&](const std::string& message){updates::peer_abort(message);updates::fail_system_upload(message);return web::error(req,message.c_str());};
    // The same receiver validates and stages local browser uploads and member
    // transfers. Neither path selects a boot partition during staging.
    if(updates::peer_begin(manifest,signature_hex,error)!=ESP_OK)return fail(error);
    if(updates::peer_chunk(release.release_id,0,std::string_view(buf.data(),first),error)!=ESP_OK)return fail(error);
    uint64_t read=first;updates::system_upload_progress(read);
    while(read<bundle.image_bytes){
        const auto count=size_t(std::min<uint64_t>(buf.size(),bundle.image_bytes-read));size_t received=0;
        while(received<count){const int n=reader.receive(buf.data()+received,count-received);if(n<=0)return fail("Firmware upload was interrupted; current firmware retained");received+=n;}
        if(updates::peer_chunk(release.release_id,read,std::string_view(buf.data(),count),error)!=ESP_OK)return fail(error);
        read+=count;updates::system_upload_progress(read);
    }
    if(updates::peer_stage(release.release_id,error)!=ESP_OK||updates::finish_system_upload(error)!=ESP_OK)return fail(error);
    httpd_resp_set_type(req,"application/json");
    return httpd_resp_sendstr(req,"{\"staged\":true}");
}
}
