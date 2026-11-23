/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The mDNS text record type the HomeKit SDK's private headers declare their
// discovery functions with. amity_hap.c reaches mDNS only through them.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *value;
} mdns_txt_item_t;

#ifdef __cplusplus
}
#endif
