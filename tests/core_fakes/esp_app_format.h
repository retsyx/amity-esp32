/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The firmware image header, with the SDK's exact packed layout.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_CHIP_ID_ESP32 = 0x0000,
    ESP_CHIP_ID_ESP32S2 = 0x0002,
    ESP_CHIP_ID_ESP32C3 = 0x0005,
    ESP_CHIP_ID_ESP32S3 = 0x0009,
    ESP_CHIP_ID_INVALID = 0xFFFF,
} __attribute__((packed)) esp_chip_id_t;

#define ESP_IMAGE_HEADER_MAGIC 0xE9
#define ESP_IMAGE_MAX_SEGMENTS 16

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint8_t spi_mode;
    uint8_t spi_speed : 4;
    uint8_t spi_size : 4;
    uint32_t entry_addr;
    uint8_t wp_pin;
    uint8_t spi_pin_drv[3];
    esp_chip_id_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t reserved[4];
    uint8_t hash_appended;
} __attribute__((packed)) esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} esp_image_segment_header_t;

#ifdef __cplusplus
static_assert(sizeof(esp_chip_id_t) == 2, "esp_chip_id_t is 16 bits");
static_assert(sizeof(esp_image_header_t) == 24, "esp_image_header_t is 24 bytes");
#else
_Static_assert(sizeof(esp_chip_id_t) == 2, "esp_chip_id_t is 16 bits");
_Static_assert(sizeof(esp_image_header_t) == 24, "esp_image_header_t is 24 bytes");
#endif

#ifdef __cplusplus
}
#endif
