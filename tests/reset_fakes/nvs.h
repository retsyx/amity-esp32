#pragma once
#include "esp_err.h"
#include <cstdint>
#include <cstddef>
using nvs_handle_t=unsigned;
enum nvs_open_mode_t {NVS_READONLY,NVS_READWRITE};
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND=10,ESP_ERR_NVS_NOT_INITIALIZED=11;
esp_err_t nvs_open(const char *,nvs_open_mode_t,nvs_handle_t *);
esp_err_t nvs_open_from_partition(const char *,const char *,nvs_open_mode_t,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
