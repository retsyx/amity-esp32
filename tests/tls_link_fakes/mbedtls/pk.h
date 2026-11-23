/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Mbed TLS private keys. A key parses only when amity_fake::tls issued it.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_ERR_PK_KEY_INVALID_FORMAT -0x3D00

typedef struct mbedtls_pk_context {
    int amity_parsed;  // set by a successful mbedtls_pk_parse_key
} mbedtls_pk_context;

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
int mbedtls_pk_parse_key(mbedtls_pk_context *ctx, const unsigned char *key, size_t keylen, const unsigned char *pwd,
                         size_t pwdlen);

#ifdef __cplusplus
}
#endif
