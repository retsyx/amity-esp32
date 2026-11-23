/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The software timer handle the HomeKit SDK's database keeps for its pairing
// mode. amity_hap.c starts no timers.
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tmrTimerControl *TimerHandle_t;

#ifdef __cplusplus
}
#endif
