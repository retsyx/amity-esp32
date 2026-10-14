/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include <cstddef>
using nvs_handle_t=unsigned;
enum nvs_open_mode_t {NVS_READONLY,NVS_READWRITE};
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND=10,ESP_ERR_NVS_TYPE_MISMATCH=12;
esp_err_t nvs_open_from_partition(const char *,const char *,nvs_open_mode_t,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
