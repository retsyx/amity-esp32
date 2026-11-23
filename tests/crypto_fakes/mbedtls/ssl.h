/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Opaque but complete Mbed TLS session and configuration types. The one
// function Amity calls outside its TLS link is a strict seam.
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mbedtls_ssl_context {
    void *private_opaque[16];
} mbedtls_ssl_context;
typedef struct mbedtls_ssl_config {
    void *private_opaque[16];
} mbedtls_ssl_config;

int mbedtls_ssl_check_pending(const mbedtls_ssl_context *ssl);

#ifdef __cplusplus
}
#endif
