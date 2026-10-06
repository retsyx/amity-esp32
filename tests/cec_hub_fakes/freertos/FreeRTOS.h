#pragma once
#include <cstdint>
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = unsigned;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr unsigned portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) (ms)
