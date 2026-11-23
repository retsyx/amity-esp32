/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Whether an address is in internal RAM, a strict seam: on the target it
// decides whether a task's stack is in internal RAM.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool esp_ptr_internal(const void *p);

#ifdef __cplusplus
}
#endif
