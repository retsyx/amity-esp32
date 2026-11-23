/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The application descriptor, with the SDK's exact layout: Amity copies it
// byte for byte out of firmware images.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_APP_DESC_MAGIC_WORD (0xABCD5432)

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full;
    uint16_t max_efuse_blk_rev_full;
    uint8_t mmu_page_size;
    uint8_t spi_flash_mode;
    uint8_t reserv3[2];
    uint32_t reserv2[18];
} esp_app_desc_t;

#ifdef __cplusplus
static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t is 256 bytes");
#else
_Static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t is 256 bytes");
#endif

// The running image's descriptor; amity_fake::app_description() edits it.
const esp_app_desc_t *esp_app_get_description(void);
int esp_app_get_elf_sha256(char *dst, size_t size);

#ifdef __cplusplus
}
#endif
