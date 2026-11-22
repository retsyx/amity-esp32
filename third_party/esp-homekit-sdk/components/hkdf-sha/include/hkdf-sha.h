/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 *
 * The subset of the RFC 6234 HKDF/SHA interface used by HomeKit pairing and
 * SRP, implemented with the platform's PSA Crypto instead of a second
 * software SHA implementation.
 */
#ifndef HKDF_SHA_H
#define HKDF_SHA_H

#include <stdint.h>
#include "psa/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { shaSuccess = 0 };
enum { SHA512HashSize = 64 };
typedef enum SHAversion { SHA512 } SHAversion;

typedef struct SHA512Context {
    psa_hash_operation_t operation;
} SHA512Context;

/* The callers cannot report a crypto failure, so a PSA failure aborts rather
 * than letting pairing continue with an unset digest or key. */
int SHA512Reset(SHA512Context *context);
int SHA512Input(SHA512Context *context, const uint8_t *bytes, unsigned int bytecount);
int SHA512Result(SHA512Context *context, uint8_t Message_Digest[SHA512HashSize]);
int hkdf(SHAversion whichSha, const unsigned char *salt, int salt_len,
         const unsigned char *ikm, int ikm_len, const unsigned char *info, int info_len,
         uint8_t okm[], int okm_len);

#ifdef __cplusplus
}
#endif

#endif /* HKDF_SHA_H */
