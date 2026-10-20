/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "system_services.hpp"
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
namespace system_services {
namespace {
EXT_RAM_BSS_ATTR std::array<char, 24576> ring{};
size_t cursor = 0, used = 0;
SemaphoreHandle_t lock=nullptr;
char boot_identifier[33]{};
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
    uint32_t entropy[4];
    bootloader_random_enable();
    esp_fill_random(entropy,sizeof(entropy));
    bootloader_random_disable();
    snprintf(boot_identifier,sizeof(boot_identifier),"%08lx%08lx%08lx%08lx",
             static_cast<unsigned long>(entropy[0]),static_cast<unsigned long>(entropy[1]),
             static_cast<unsigned long>(entropy[2]),static_cast<unsigned long>(entropy[3]));
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
    if(result==ESP_OK){ready.store(true);updates::local_boot_healthy();}
    return result;
}
esp_err_t upload_firmware(httpd_req_t *req) {
    const auto *target=esp_ota_get_next_update_partition(nullptr);
    constexpr size_t header_size=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    constexpr size_t metadata_cap=updates::update_bundle_header_bytes+8192+64;
    if(!target||req->content_len<header_size||uint64_t(req->content_len)>uint64_t(target->size)+metadata_cap)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid firmware size");
    std::array<uint8_t,2048> buf{};
    const auto metadata_deadline=esp_timer_get_time()+30000000;
    auto read_exact=[&](char* destination,size_t count){
        size_t received=0;while(received<count&&esp_timer_get_time()<metadata_deadline){const int n=httpd_req_recv(req,destination+received,count-received);if(n<=0)return false;received+=n;}return received==count;
    };
    if(!read_exact(reinterpret_cast<char*>(buf.data()),updates::update_bundle_header_bytes))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete firmware header");
    uint64_t image_bytes=static_cast<uint64_t>(req->content_len);size_t read=updates::update_bundle_header_bytes;
    auto release=updates::staged_release();
    const std::string_view prefix(reinterpret_cast<const char*>(buf.data()),updates::update_bundle_header_bytes);
    if(prefix.substr(0,updates::update_bundle_magic.size())==updates::update_bundle_magic){
        updates::UpdateBundleHeader bundle;std::string error;
        if(!updates::parse_update_bundle_header(prefix,req->content_len,bundle,error))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,error.c_str());
        std::string manifest(bundle.manifest_bytes,'\0'),signature(bundle.signature_bytes,'\0');
        if(!read_exact(manifest.data(),manifest.size())||!read_exact(signature.data(),signature.size()))
            return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete update bundle metadata");
        std::string signature_hex;signature_hex.reserve(signature.size()*2);constexpr char digits[]="0123456789abcdef";
        for(unsigned char byte:signature){signature_hex+=digits[byte>>4];signature_hex+=digits[byte&15];}
        // Publisher/profile/hardware/rollback checks run before any slot erase.
        if(updates::stage_release(manifest,signature_hex,error)!=ESP_OK)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,error.c_str());
        release=updates::staged_release();image_bytes=bundle.image_bytes;read=0;
    }
    if(!release)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Select the complete Amity update file, or validate the release manifest before uploading a raw application image");
    if(image_bytes<header_size||image_bytes>target->size||image_bytes!=release->image_bytes)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid firmware size");
    // Validate the complete native application header before erasing the slot.
    if(!read_exact(reinterpret_cast<char*>(buf.data()+read),header_size-read))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete firmware header");
    read=header_size;
    esp_image_header_t image{};
    esp_app_desc_t app{};
    memcpy(&image,buf.data(),sizeof(image));
    memcpy(&app,buf.data()+sizeof(image)+sizeof(esp_image_segment_header_t),sizeof(app));
    const esp_app_desc_t *running=esp_app_get_description();
    if(image.magic!=ESP_IMAGE_HEADER_MAGIC || image.chip_id!=CONFIG_IDF_FIRMWARE_CHIP_ID ||
       app.magic_word!=ESP_APP_DESC_MAGIC_WORD || memcmp(app.project_name,running->project_name,sizeof(app.project_name))!=0)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Firmware is not for this Amity target");
    if(!memchr(app.version,'\0',sizeof(app.version)))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid firmware version field");
    char elf_identity[65]{};for(size_t n=0;n<sizeof(app.app_elf_sha256);++n)snprintf(elf_identity+2*n,3,"%02x",app.app_elf_sha256[n]);
    if(release->firmware_version!=app.version||release->image_elf_sha256!=elf_identity)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Application identity does not match the validated release");
    const auto comparison=compare_firmware_versions(app.version,running->version);
    if(!comparison||*comparison<0) {
        // Bind approval to the running version, so a stale browser cannot silently approve a downgrade.
        char confirmed[sizeof(running->version)]{};
        if(httpd_req_get_hdr_value_str(req,"X-Amity-Confirm-Version",confirmed,sizeof(confirmed))!=ESP_OK||
           strcmp(confirmed,running->version)!=0) {
            httpd_resp_set_status(req,"409 Conflict");
            httpd_resp_set_type(req,"application/json");
            return httpd_resp_sendstr(req,comparison?
                "{\"error\":\"Firmware downgrade requires explicit confirmation. Reload the page and select the file again.\"}":
                "{\"error\":\"Firmware versions cannot be compared. Reload the page and select the file again to confirm.\"}");
        }
    }
    std::string maintenance_error;
    if(updates::begin_install(maintenance_error)!=ESP_OK)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,maintenance_error.c_str());
    psa_hash_operation_t digest=PSA_HASH_OPERATION_INIT;
    if(psa_hash_setup(&digest,PSA_ALG_SHA_256)!=PSA_SUCCESS){updates::install_failed("Image digest initialization failed");return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Image digest initialization failed");}
    esp_ota_handle_t handle;
    esp_err_t e=esp_ota_begin(target,image_bytes,&handle);
    if(e!=ESP_OK){psa_hash_abort(&digest);updates::install_failed("Inactive image could not be opened");return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,esp_err_to_name(e));}
    e=esp_ota_write(handle,buf.data(),read);
    if(e==ESP_OK&&psa_hash_update(&digest,buf.data(),read)!=PSA_SUCCESS)e=ESP_FAIL;
    while(e==ESP_OK && read<image_bytes) {
        int n=httpd_req_recv(req,reinterpret_cast<char *>(buf.data()),std::min<uint64_t>(buf.size(),image_bytes-read));
        if(n<=0) { e=ESP_ERR_TIMEOUT; break; }
        e=esp_ota_write(handle,buf.data(),n);
        if(e==ESP_OK&&psa_hash_update(&digest,buf.data(),n)!=PSA_SUCCESS)e=ESP_FAIL;
        read+=n;
        updates::install_progress(read);
    }
    uint8_t hash[32];size_t hash_size=0;char hash_text[65]{};
    if(e==ESP_OK&&(psa_hash_finish(&digest,hash,sizeof(hash),&hash_size)!=PSA_SUCCESS||hash_size!=32))e=ESP_FAIL;
    psa_hash_abort(&digest);
    if(e==ESP_OK){for(size_t n=0;n<sizeof(hash);++n)snprintf(hash_text+2*n,3,"%02x",hash[n]);if(release->image_sha256!=hash_text)e=ESP_ERR_INVALID_CRC;}
    if(e!=ESP_OK) { esp_ota_abort(handle);updates::install_failed("Firmware stream or image digest failed; current image retained");return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Firmware upload failed; current firmware retained"); }
    e=esp_ota_end(handle);
    if(e==ESP_OK)e=updates::install_selected();
    if(e==ESP_OK) e=esp_ota_set_boot_partition(target);
    if(e!=ESP_OK){updates::install_failed("Firmware validation or activation failed");return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Firmware validation failed");}
    httpd_resp_set_type(req,"application/json");
    e=httpd_resp_sendstr(req,"{\"rebooting\":true}");
    reboot_later();
    return e;
}
}
