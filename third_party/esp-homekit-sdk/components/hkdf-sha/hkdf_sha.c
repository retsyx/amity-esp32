/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 */
#include "hkdf-sha.h"

#include <stdlib.h>

static void check(psa_status_t status)
{
    if (status != PSA_SUCCESS) {
        abort();
    }
}

int SHA512Reset(SHA512Context *context)
{
    check(psa_crypto_init());
    context->operation = psa_hash_operation_init();
    check(psa_hash_setup(&context->operation, PSA_ALG_SHA_512));
    return shaSuccess;
}

int SHA512Input(SHA512Context *context, const uint8_t *bytes, unsigned int bytecount)
{
    check(psa_hash_update(&context->operation, bytes, bytecount));
    return shaSuccess;
}

int SHA512Result(SHA512Context *context, uint8_t Message_Digest[SHA512HashSize])
{
    size_t length = 0;
    check(psa_hash_finish(&context->operation, Message_Digest, SHA512HashSize, &length));
    if (length != SHA512HashSize) {
        abort();
    }
    return shaSuccess;
}

int hkdf(SHAversion whichSha, const unsigned char *salt, int salt_len,
         const unsigned char *ikm, int ikm_len, const unsigned char *info, int info_len,
         uint8_t okm[], int okm_len)
{
    if (whichSha != SHA512 || salt_len < 0 || ikm_len < 0 || info_len < 0 || okm_len < 0) {
        abort();
    }
    check(psa_crypto_init());
    psa_key_derivation_operation_t operation = psa_key_derivation_operation_init();
    check(psa_key_derivation_setup(&operation, PSA_ALG_HKDF(PSA_ALG_SHA_512)));
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt, (size_t)salt_len));
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, (size_t)ikm_len));
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_INFO, info, (size_t)info_len));
    check(psa_key_derivation_output_bytes(&operation, okm, (size_t)okm_len));
    check(psa_key_derivation_abort(&operation));
    return shaSuccess;
}
