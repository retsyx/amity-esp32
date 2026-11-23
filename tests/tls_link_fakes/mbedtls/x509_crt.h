/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Mbed TLS certificate chains: each certificate's raw DER and chain link,
// which the TLS link reads, with the SDK's error codes and verification flags.
// A chain parses only certificates that amity_fake::tls issued.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_ERR_X509_INVALID_FORMAT -0x2180
#define MBEDTLS_ERR_X509_CERT_VERIFY_FAILED -0x2700

#define MBEDTLS_X509_BADCERT_EXPIRED 0x01
#define MBEDTLS_X509_BADCERT_NOT_TRUSTED 0x08
#define MBEDTLS_X509_BADCERT_OTHER 0x0100
#define MBEDTLS_X509_BADCERT_FUTURE 0x0200

typedef struct mbedtls_asn1_buf {
    int tag;
    size_t len;
    unsigned char *p;
} mbedtls_asn1_buf;
typedef mbedtls_asn1_buf mbedtls_x509_buf;

typedef struct mbedtls_x509_crt {
    mbedtls_x509_buf raw;
    struct mbedtls_x509_crt *next;
} mbedtls_x509_crt;

// Revocation lists stay opaque: the link configures none.
typedef struct mbedtls_x509_crl mbedtls_x509_crl;

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt);
void mbedtls_x509_crt_free(mbedtls_x509_crt *crt);
int mbedtls_x509_crt_parse_der(mbedtls_x509_crt *chain, const unsigned char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
