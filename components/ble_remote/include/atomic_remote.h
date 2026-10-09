/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ATOMIC_CEC_KEY, ATOMIC_ACTIVITY } atomic_action_type_t;
typedef struct {
    atomic_action_type_t type;
    uint8_t value; // CEC user-control code, or zero-based activity index.
} atomic_action_t;
bool atomic_remote_translate(uint16_t usage_page, uint16_t usage, atomic_action_t *action);
#ifdef __cplusplus
}
#endif
