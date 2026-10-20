#pragma once
#include "esp_err.h"
#include <cstddef>
struct esp_partition_t {};
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_ANY=255;
const esp_partition_t* esp_partition_find_first(int,int,const char*);
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t);
