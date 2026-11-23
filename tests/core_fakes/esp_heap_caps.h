/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Allocations come from malloc and remember their capabilities, which
// amity_fake::allocation_caps() reports. amity_fake::fail_allocations()
// scripts failures, which call the registered failed-allocation hook.
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)
#define MALLOC_CAP_IRAM_8BIT (1 << 13)
#define MALLOC_CAP_RETENTION (1 << 14)
#define MALLOC_CAP_RTCRAM (1 << 15)

typedef struct {
    size_t total_free_bytes;
    size_t total_allocated_bytes;
    size_t largest_free_block;
    size_t minimum_free_bytes;
    size_t allocated_blocks;
    size_t free_blocks;
    size_t total_blocks;
} multi_heap_info_t;

typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char *function_name);

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
// Recorded; amity_fake::extmem_limit() reports the limit.
void heap_caps_malloc_extmem_enable(size_t limit);
// Strict seams: install a behaviour with amity_seam::install.
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps);

#ifdef __cplusplus
}
#endif
