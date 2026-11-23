/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Mbed TLS secure memory wiping.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_platform_zeroize(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
