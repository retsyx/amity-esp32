/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// PSA Crypto backed by OpenSSL, with the pinned TF-PSA-Crypto's types and
// constants. It implements what Amity uses: P-256 key pairs and ECDH,
// HKDF-SHA-256, AES-GCM, SHA-256 and random bytes. As in the SDK, calls fail
// before psa_crypto_init and outside a key's usage policy; a key ID that does
// not exist fails the scenario.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t psa_status_t;
typedef uint16_t psa_key_type_t;
typedef uint8_t psa_ecc_family_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_id_t;
typedef psa_key_id_t mbedtls_svc_key_id_t;
typedef uint32_t psa_key_usage_t;
typedef uint16_t psa_key_derivation_step_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-132)
#define PSA_ERROR_NOT_PERMITTED ((psa_status_t)-133)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)
#define PSA_ERROR_INVALID_ARGUMENT ((psa_status_t)-135)
#define PSA_ERROR_INVALID_HANDLE ((psa_status_t)-136)
#define PSA_ERROR_BAD_STATE ((psa_status_t)-137)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)
#define PSA_ERROR_INSUFFICIENT_MEMORY ((psa_status_t)-141)
#define PSA_ERROR_INVALID_SIGNATURE ((psa_status_t)-149)

#define PSA_KEY_ID_NULL ((psa_key_id_t)0)
#define PSA_KEY_TYPE_NONE ((psa_key_type_t)0x0000)
#define PSA_KEY_TYPE_AES ((psa_key_type_t)0x2400)
#define PSA_KEY_TYPE_ECC_KEY_PAIR_BASE ((psa_key_type_t)0x7100)
#define PSA_KEY_TYPE_ECC_KEY_PAIR(curve) (PSA_KEY_TYPE_ECC_KEY_PAIR_BASE | (curve))
#define PSA_ECC_FAMILY_SECP_R1 ((psa_ecc_family_t)0x12)
#define PSA_ALG_HASH_MASK ((psa_algorithm_t)0x000000ff)
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
#define PSA_ALG_GCM ((psa_algorithm_t)0x05500200)
#define PSA_ALG_HKDF_BASE ((psa_algorithm_t)0x08000100)
#define PSA_ALG_HKDF(hash_alg) (PSA_ALG_HKDF_BASE | ((hash_alg) & PSA_ALG_HASH_MASK))
#define PSA_ALG_ECDH ((psa_algorithm_t)0x09020000)
#define PSA_KEY_USAGE_ENCRYPT ((psa_key_usage_t)0x00000100)
#define PSA_KEY_USAGE_DECRYPT ((psa_key_usage_t)0x00000200)
#define PSA_KEY_USAGE_DERIVE ((psa_key_usage_t)0x00004000)
#define PSA_KEY_DERIVATION_INPUT_SECRET ((psa_key_derivation_step_t)0x0101)
#define PSA_KEY_DERIVATION_INPUT_SALT ((psa_key_derivation_step_t)0x0202)
#define PSA_KEY_DERIVATION_INPUT_INFO ((psa_key_derivation_step_t)0x0203)

typedef struct {
    psa_key_type_t type;
    size_t bits;
    psa_key_usage_t usage;
    psa_algorithm_t alg;
} psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT {PSA_KEY_TYPE_NONE, 0, 0, 0}
static inline void psa_set_key_type(psa_key_attributes_t *attributes, psa_key_type_t type) { attributes->type = type; }
static inline void psa_set_key_bits(psa_key_attributes_t *attributes, size_t bits) { attributes->bits = bits; }
static inline void psa_set_key_usage_flags(psa_key_attributes_t *attributes, psa_key_usage_t usage_flags) {
    attributes->usage = usage_flags;
}
static inline void psa_set_key_algorithm(psa_key_attributes_t *attributes, psa_algorithm_t alg) {
    attributes->alg = alg;
}

// The inputs of a derivation live in the fake until psa_key_derivation_abort.
typedef struct {
    psa_algorithm_t alg;
    uint32_t id;
} psa_key_derivation_operation_t;
#define PSA_KEY_DERIVATION_OPERATION_INIT {0, 0}

psa_status_t psa_crypto_init(void);
psa_status_t psa_generate_key(const psa_key_attributes_t *attributes, mbedtls_svc_key_id_t *key);
psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data, size_t data_length,
                            mbedtls_svc_key_id_t *key);
psa_status_t psa_export_public_key(mbedtls_svc_key_id_t key, uint8_t *data, size_t data_size, size_t *data_length);
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key);
psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length);
psa_status_t psa_generate_random(uint8_t *output, size_t output_size);
psa_status_t psa_raw_key_agreement(psa_algorithm_t alg, mbedtls_svc_key_id_t private_key, const uint8_t *peer_key,
                                   size_t peer_key_length, uint8_t *output, size_t output_size, size_t *output_length);
psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *operation, psa_algorithm_t alg);
psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *operation, psa_key_derivation_step_t step,
                                            const uint8_t *data, size_t data_length);
psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *operation, uint8_t *output,
                                             size_t output_length);
psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *operation);
psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce, size_t nonce_length,
                              const uint8_t *additional_data, size_t additional_data_length, const uint8_t *plaintext,
                              size_t plaintext_length, uint8_t *ciphertext, size_t ciphertext_size,
                              size_t *ciphertext_length);
psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce, size_t nonce_length,
                              const uint8_t *additional_data, size_t additional_data_length, const uint8_t *ciphertext,
                              size_t ciphertext_length, uint8_t *plaintext, size_t plaintext_size,
                              size_t *plaintext_length);

#ifdef __cplusplus
}
#endif
