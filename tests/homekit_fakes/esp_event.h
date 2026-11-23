/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The event base declaration hap.h uses. Amity's HomeKit code posts and
// handles no events.
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *esp_event_base_t;
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id

#ifdef __cplusplus
}
#endif
