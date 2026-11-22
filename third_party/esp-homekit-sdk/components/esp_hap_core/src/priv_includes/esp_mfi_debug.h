/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS products only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef ESP_MFI_DEBUG_H_
#define ESP_MFI_DEBUG_H_

#include <stdint.h>
#include <inttypes.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C"{
#endif

/* Message levels. HomeKit messages use ESP-IDF logging, so they reach the log
 * capture and follow the configured log level. */
#define ESP_MFI_DEBUG_INFO      1
#define ESP_MFI_DEBUG_WARN      2
#define ESP_MFI_DEBUG_ERR       3
#define ESP_MFI_DEBUG_ASSERT    4
#define ESP_MFI_DEBUG_BLOCK     5

#define ESP_MFI_DEBUG(l, fmt, ...)                                                          \
    do {                                                                                    \
        if ((l) >= ESP_MFI_DEBUG_ERR) {                                                     \
            ESP_LOGE("hap", fmt, ##__VA_ARGS__);                                            \
        } else if ((l) == ESP_MFI_DEBUG_WARN) {                                             \
            ESP_LOGW("hap", fmt, ##__VA_ARGS__);                                            \
        } else {                                                                            \
            ESP_LOGI("hap", fmt, ##__VA_ARGS__);                                            \
        }                                                                                   \
    } while (0)

/* A failed assertion is logged, then blocks the calling task. */
#define ESP_MFI_ASSERT(cond)                                                                \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            ESP_LOGE("hap", "Assertion failed at %s:%d", __FILE__, __LINE__);               \
            while (1);                                                                      \
        }                                                                                   \
    } while (0)

/**
 * @bref check if data length matches given length
 *
 * @param optlen data length
 * @param opttype data
 *
 * @return none
 *
 * void ESP_MFI_CHECK_OPTLEN(int conditions, (all type) data);
 */
#define ESP_MFI_CHECK_OPTLEN(optlen, opttype)                                               \
    do {                                                                                    \
        if ((optlen) > sizeof(opttype)) {                                                   \
            return -1;                                                                      \
        }                                                                                   \
    } while(0)                                                                              \

#ifdef __cplusplus
}
#endif

#endif /* ESP_MFI_DEBUG_H_ */
