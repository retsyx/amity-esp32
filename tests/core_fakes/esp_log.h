/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// ESP_LOGx compile their arguments against the format, as on the target, and
// esp_log_write records each line for amity_fake::log_lines(). Debug and
// verbose lines compile but never run: the Amity build's maximum level is info.
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR = 1,
    ESP_LOG_WARN = 2,
    ESP_LOG_INFO = 3,
    ESP_LOG_DEBUG = 4,
    ESP_LOG_VERBOSE = 5,
} esp_log_level_t;

typedef int (*vprintf_like_t)(const char *, va_list);

// Lines go to func as "I (<ms>) <tag>: <message>\n", by default to stdout.
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
void esp_log_level_set(const char *tag, esp_log_level_t level);
// Milliseconds on the scripted clock.
uint32_t esp_log_timestamp(void);
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...) __attribute__((format(printf, 3, 4)));
void esp_log_writev(esp_log_level_t level, const char *tag, const char *format, va_list args);

#ifdef __cplusplus
}
#endif

#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#endif

#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) \
    do {                                             \
        if (LOG_LOCAL_LEVEL >= (level)) esp_log_write((level), (tag), format, ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGE(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, format, ##__VA_ARGS__)
