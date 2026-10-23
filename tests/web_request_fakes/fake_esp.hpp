/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <map>
#include <string>

using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=1;
#define ESP_ERROR_CHECK(expression) do {if((expression)!=ESP_OK)std::abort();} while(0)
inline const char *esp_err_to_name(esp_err_t) {return "FAKE_ERROR";}
using httpd_handle_t=void *;
enum httpd_method_t {HTTP_GET,HTTP_POST};
constexpr int HTTPD_SOCK_ERR_TIMEOUT=-3;
struct httpd_req_t {
    size_t content_len=0;
    int socket=7;
    std::string input,status="200 OK",response;
    std::map<std::string,std::string> headers,request_headers;
    size_t offset=0,chunk=2048,received=0;
    int receive_calls=0;
    bool socket_input=false,wait_record=false;
};
int httpd_req_to_sockfd(httpd_req_t *);
int httpd_req_recv(httpd_req_t *,char *,size_t);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *,const char *,char *,size_t);
esp_err_t httpd_resp_set_status(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
esp_err_t httpd_resp_send(httpd_req_t *,const char *,size_t);
esp_err_t httpd_resp_sendstr(httpd_req_t *,const char *);

struct esp_timer;
using esp_timer_handle_t=esp_timer *;
struct esp_timer_create_args_t {void (*callback)(void *);void *arg;const char *name;};
int64_t esp_timer_get_time();
esp_err_t esp_timer_create(const esp_timer_create_args_t *,esp_timer_handle_t *);
esp_err_t esp_timer_start_once(esp_timer_handle_t,uint64_t);
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t,uint32_t);
esp_err_t esp_timer_delete(esp_timer_handle_t);

using TickType_t=uint32_t;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(TickType_t) {}
extern unsigned fake_reboots;
inline int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *) {++fake_reboots;return 1;}
using SemaphoreHandle_t=void *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {return reinterpret_cast<void *>(1);}
inline int xSemaphoreTake(SemaphoreHandle_t,TickType_t) {return 1;}
inline int xSemaphoreGive(SemaphoreHandle_t) {return 1;}
using vprintf_like_t=int (*)(const char *,va_list);
inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn) {return fn;}
#define ESP_LOGI(...) do {} while(0)
#define EXT_RAM_BSS_ATTR
inline uint32_t esp_random() {return 123;}
inline void esp_restart() {}
constexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4;
struct multi_heap_info_t {size_t total_free_bytes=0,minimum_free_bytes=0,largest_free_block=0;};
inline void heap_caps_get_info(multi_heap_info_t *,unsigned) {}

constexpr uint8_t ESP_IMAGE_HEADER_MAGIC=0xe9;
constexpr uint32_t ESP_APP_DESC_MAGIC_WORD=0xabcd5432;
constexpr uint16_t CONFIG_IDF_FIRMWARE_CHIP_ID=9;
struct esp_image_header_t {uint8_t magic;uint16_t chip_id;};
struct esp_image_segment_header_t {uint32_t address,length;};
struct esp_app_desc_t {uint32_t magic_word;char version[32],project_name[32];};
const esp_app_desc_t *esp_app_get_description();
struct esp_partition_t {size_t size;};
using esp_ota_handle_t=unsigned;
enum esp_ota_img_states_t {ESP_OTA_IMG_VALID,ESP_OTA_IMG_PENDING_VERIFY};
const esp_partition_t *esp_ota_get_running_partition();
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *,esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
esp_err_t esp_ota_begin(const esp_partition_t *,size_t,esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t,const void *,size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
