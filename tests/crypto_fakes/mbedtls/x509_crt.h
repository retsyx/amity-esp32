/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// An opaque but complete Mbed TLS certificate chain.
#include "mbedtls/pk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mbedtls_x509_crt {
    void *private_opaque[8];
} mbedtls_x509_crt;

#ifdef __cplusplus
}
#endif
