/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shutdown_handler_t)(void);

// Throws amity_fake::Restart: nothing after a restart runs.
void esp_restart(void) __attribute__((__noreturn__));
// Recorded; amity_fake::shutdown_handlers() lists them.
esp_err_t esp_register_shutdown_handler(shutdown_handler_t handle);
// Strict seams: install a behaviour with amity_seam::install.
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);

#ifdef __cplusplus
}
#endif
