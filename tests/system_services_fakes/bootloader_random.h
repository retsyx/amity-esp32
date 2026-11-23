/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The bootloader entropy source. Strict seams: a scenario installs each
// behaviour with amity_seam::install.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void bootloader_random_enable(void);
void bootloader_random_disable(void);

#ifdef __cplusplus
}
#endif
