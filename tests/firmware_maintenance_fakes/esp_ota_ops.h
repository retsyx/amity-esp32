#pragma once
#include "esp_err.h"
#include "esp_partition.h"
#include <cstddef>
#include <cstdint>
using esp_ota_handle_t=uint32_t;
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_begin(const esp_partition_t*,size_t,esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t,const void*,size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition_skip_validate(const esp_partition_t*);
