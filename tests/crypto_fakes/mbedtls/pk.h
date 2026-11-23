/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// An opaque but complete Mbed TLS key, so Amity types can hold one by value.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mbedtls_pk_context {
    void *private_opaque[4];
} mbedtls_pk_context;

#ifdef __cplusplus
}
#endif
