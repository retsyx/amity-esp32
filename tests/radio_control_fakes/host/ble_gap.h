/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Whether NimBLE's advertiser is still running. As in NimBLE, the header
// brings in the build configuration.
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

int ble_gap_adv_active(void);

#ifdef __cplusplus
}
#endif
