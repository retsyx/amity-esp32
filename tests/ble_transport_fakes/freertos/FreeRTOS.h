#pragma once
#include <stdint.h>
using BaseType_t = int;
using TickType_t = uint32_t;
constexpr BaseType_t pdFALSE = 0, pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
