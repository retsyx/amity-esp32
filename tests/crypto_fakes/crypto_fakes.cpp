/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// PSA Crypto over OpenSSL, and the Mbed TLS functions Amity calls outside its
// TLS link.
#include "crypto_fakes.hpp"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"
#include "seams.hpp"
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

constexpr size_t public_key_size = 65;  // an uncompressed P-256 point
constexpr size_t tag_size = 16;

struct Key {
    psa_key_attributes_t attributes{};
    EVP_PKEY *pair = nullptr;
    std::vector<uint8_t> secret;
};
struct Derivation {
    std::vector<uint8_t> salt, secret, info;
    bool has_secret = false, has_info = false;
};

std::mutex lock;
bool initialized = false;
std::map<psa_key_id_t, Key> keys;
psa_key_id_t next_key = 0;
std::map<uint32_t, Derivation> derivations;
uint32_t next_derivation = 0;
unsigned generation_failures = 0;

Key &key(psa_key_id_t id, const char *function) {
    const auto found = keys.find(id);
    if (found == keys.end()) fail(std::string(function) + " used a PSA key that does not exist");
    return found->second;
}

Derivation *derivation(const psa_key_derivation_operation_t *operation) {
    const auto found = derivations.find(operation->id);
    return found == derivations.end() ? nullptr : &found->second;
}

void wipe(std::vector<uint8_t> &bytes) {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    bytes.clear();
}

psa_key_id_t store(Key value) {
    keys[++next_key] = std::move(value);
    return next_key;
}

const EVP_CIPHER *gcm(size_t key_bytes) {
    switch (key_bytes) {
    case 16: return EVP_aes_128_gcm();
    case 24: return EVP_aes_192_gcm();
    case 32: return EVP_aes_256_gcm();
    default: return nullptr;
    }
}

psa_status_t check_aead(const Key &value, psa_algorithm_t alg, psa_key_usage_t usage, size_t nonce_length) {
    if (alg != PSA_ALG_GCM || nonce_length != 12) return PSA_ERROR_NOT_SUPPORTED;
    if (value.attributes.type != PSA_KEY_TYPE_AES) return PSA_ERROR_INVALID_ARGUMENT;
    if (!(value.attributes.usage & usage) || value.attributes.alg != PSA_ALG_GCM) return PSA_ERROR_NOT_PERMITTED;
    return PSA_SUCCESS;
}

}  // namespace

namespace amity_fake {

void fail_key_generations(unsigned count) {
    std::lock_guard<std::mutex> guard(lock);
    generation_failures = count;
}
std::size_t psa_keys() {
    std::lock_guard<std::mutex> guard(lock);
    return keys.size();
}
std::size_t psa_derivations() {
    std::lock_guard<std::mutex> guard(lock);
    return derivations.size();
}

}  // namespace amity_fake

extern "C" {

void mbedtls_platform_zeroize(void *buf, size_t len) {
    if (buf && len) OPENSSL_cleanse(buf, len);
}

int mbedtls_ssl_check_pending(const mbedtls_ssl_context *ssl) { return AMITY_SEAM(mbedtls_ssl_check_pending, ssl); }

psa_status_t psa_crypto_init(void) {
    std::lock_guard<std::mutex> guard(lock);
    initialized = true;
    return PSA_SUCCESS;
}

psa_status_t psa_generate_key(const psa_key_attributes_t *attributes, mbedtls_svc_key_id_t *key_id) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    if (generation_failures) {
        --generation_failures;
        return PSA_ERROR_INSUFFICIENT_MEMORY;
    }
    if (attributes->type != PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) || attributes->bits != 256)
        return PSA_ERROR_NOT_SUPPORTED;
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY *pair = nullptr;
    const bool good = context && EVP_PKEY_keygen_init(context) > 0 &&
                      EVP_PKEY_CTX_set_group_name(context, "prime256v1") > 0 && EVP_PKEY_generate(context, &pair) > 0;
    EVP_PKEY_CTX_free(context);
    if (!good) return PSA_ERROR_GENERIC_ERROR;
    *key_id = store(Key{*attributes, pair, {}});
    return PSA_SUCCESS;
}

psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data, size_t data_length,
                            mbedtls_svc_key_id_t *key_id) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    if (attributes->type != PSA_KEY_TYPE_AES) return PSA_ERROR_NOT_SUPPORTED;
    if (!gcm(data_length) || (attributes->bits && attributes->bits != data_length * 8)) return PSA_ERROR_INVALID_ARGUMENT;
    Key value{*attributes, nullptr, std::vector<uint8_t>(data, data + data_length)};
    value.attributes.bits = data_length * 8;
    *key_id = store(std::move(value));
    return PSA_SUCCESS;
}

psa_status_t psa_export_public_key(mbedtls_svc_key_id_t key_id, uint8_t *data, size_t data_size, size_t *data_length) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    const Key &value = key(key_id, "psa_export_public_key");
    if (!value.pair) return PSA_ERROR_INVALID_ARGUMENT;
    if (data_size < public_key_size) return PSA_ERROR_BUFFER_TOO_SMALL;
    return EVP_PKEY_get_octet_string_param(value.pair, OSSL_PKEY_PARAM_PUB_KEY, data, data_size, data_length) > 0
               ? PSA_SUCCESS
               : PSA_ERROR_GENERIC_ERROR;
}

psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key_id) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    if (key_id == PSA_KEY_ID_NULL) return PSA_SUCCESS;
    Key &value = key(key_id, "psa_destroy_key");
    EVP_PKEY_free(value.pair);
    wipe(value.secret);
    keys.erase(key_id);
    return PSA_SUCCESS;
}

psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length) {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!initialized) return PSA_ERROR_BAD_STATE;
    }
    if (alg != PSA_ALG_SHA_256) return PSA_ERROR_NOT_SUPPORTED;
    if (hash_size < 32) return PSA_ERROR_BUFFER_TOO_SMALL;
    unsigned size = 0;
    if (EVP_Digest(input, input_length, hash, &size, EVP_sha256(), nullptr) <= 0) return PSA_ERROR_GENERIC_ERROR;
    *hash_length = size;
    return PSA_SUCCESS;
}

psa_status_t psa_generate_random(uint8_t *output, size_t output_size) {
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!initialized) return PSA_ERROR_BAD_STATE;
    }
    return output_size == 0 || RAND_bytes(output, int(output_size)) > 0 ? PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR;
}

psa_status_t psa_raw_key_agreement(psa_algorithm_t alg, mbedtls_svc_key_id_t private_key, const uint8_t *peer_key,
                                   size_t peer_key_length, uint8_t *output, size_t output_size, size_t *output_length) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    const Key &value = key(private_key, "psa_raw_key_agreement");
    if (alg != PSA_ALG_ECDH) return PSA_ERROR_NOT_SUPPORTED;
    if (!value.pair) return PSA_ERROR_INVALID_ARGUMENT;
    if (!(value.attributes.usage & PSA_KEY_USAGE_DERIVE) || value.attributes.alg != PSA_ALG_ECDH)
        return PSA_ERROR_NOT_PERMITTED;
    if (peer_key_length != public_key_size || peer_key[0] != 4) return PSA_ERROR_INVALID_ARGUMENT;
    if (output_size < 32) return PSA_ERROR_BUFFER_TOO_SMALL;
    char group[] = "prime256v1";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, const_cast<uint8_t *>(peer_key), peer_key_length),
        OSSL_PARAM_construct_end()};
    EVP_PKEY_CTX *parse = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY *peer = nullptr;
    bool good = parse && EVP_PKEY_fromdata_init(parse) > 0 &&
                EVP_PKEY_fromdata(parse, &peer, EVP_PKEY_PUBLIC_KEY, parameters) > 0;
    EVP_PKEY_CTX_free(parse);
    EVP_PKEY_CTX *derive = good ? EVP_PKEY_CTX_new(value.pair, nullptr) : nullptr;
    size_t length = output_size;
    good = good && derive && EVP_PKEY_derive_init(derive) > 0 && EVP_PKEY_derive_set_peer(derive, peer) > 0 &&
           EVP_PKEY_derive(derive, output, &length) > 0;
    EVP_PKEY_CTX_free(derive);
    EVP_PKEY_free(peer);
    if (!good) return PSA_ERROR_INVALID_ARGUMENT;
    *output_length = length;
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *operation, psa_algorithm_t alg) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    if (operation->id) return PSA_ERROR_BAD_STATE;
    if (alg != PSA_ALG_HKDF(PSA_ALG_SHA_256)) return PSA_ERROR_NOT_SUPPORTED;
    operation->alg = alg;
    operation->id = ++next_derivation;
    derivations[operation->id] = Derivation{};
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *operation, psa_key_derivation_step_t step,
                                            const uint8_t *data, size_t data_length) {
    std::lock_guard<std::mutex> guard(lock);
    Derivation *value = derivation(operation);
    if (!value) return PSA_ERROR_BAD_STATE;
    const std::vector<uint8_t> bytes(data, data + data_length);
    // HKDF takes the salt first, then the secret, then the info.
    switch (step) {
    case PSA_KEY_DERIVATION_INPUT_SALT:
        if (value->has_secret || !value->salt.empty()) return PSA_ERROR_BAD_STATE;
        value->salt = bytes;
        return PSA_SUCCESS;
    case PSA_KEY_DERIVATION_INPUT_SECRET:
        if (value->has_secret) return PSA_ERROR_BAD_STATE;
        value->secret = bytes;
        value->has_secret = true;
        return PSA_SUCCESS;
    case PSA_KEY_DERIVATION_INPUT_INFO:
        if (!value->has_secret || value->has_info) return PSA_ERROR_BAD_STATE;
        value->info = bytes;
        value->has_info = true;
        return PSA_SUCCESS;
    default: return PSA_ERROR_INVALID_ARGUMENT;
    }
}

psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *operation, uint8_t *output,
                                             size_t output_length) {
    std::lock_guard<std::mutex> guard(lock);
    Derivation *value = derivation(operation);
    if (!value || !value->has_secret) return PSA_ERROR_BAD_STATE;
    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    EVP_KDF_CTX *context = kdf ? EVP_KDF_CTX_new(kdf) : nullptr;
    char digest[] = "SHA256";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, value->secret.data(), value->secret.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, value->salt.data(), value->salt.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, value->info.data(), value->info.size()),
        OSSL_PARAM_construct_end()};
    const bool good = context && EVP_KDF_derive(context, output, output_length, parameters) > 0;
    EVP_KDF_CTX_free(context);
    EVP_KDF_free(kdf);
    return good ? PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR;
}

psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *operation) {
    std::lock_guard<std::mutex> guard(lock);
    if (Derivation *value = derivation(operation)) {
        wipe(value->salt);
        wipe(value->secret);
        wipe(value->info);
        derivations.erase(operation->id);
    }
    operation->id = 0;
    return PSA_SUCCESS;
}

psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key_id, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data, size_t additional_data_length,
                              const uint8_t *plaintext, size_t plaintext_length, uint8_t *ciphertext,
                              size_t ciphertext_size, size_t *ciphertext_length) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    const Key &value = key(key_id, "psa_aead_encrypt");
    if (const psa_status_t status = check_aead(value, alg, PSA_KEY_USAGE_ENCRYPT, nonce_length)) return status;
    if (ciphertext_size < plaintext_length + tag_size) return PSA_ERROR_BUFFER_TOO_SMALL;
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int used = 0, tail = 0;
    const bool good =
        context && EVP_EncryptInit_ex(context, gcm(value.secret.size()), nullptr, value.secret.data(), nonce) > 0 &&
        EVP_EncryptUpdate(context, nullptr, &used, additional_data, int(additional_data_length)) > 0 &&
        EVP_EncryptUpdate(context, ciphertext, &used, plaintext, int(plaintext_length)) > 0 &&
        EVP_EncryptFinal_ex(context, ciphertext + used, &tail) > 0 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, int(tag_size), ciphertext + used + tail) > 0;
    EVP_CIPHER_CTX_free(context);
    if (!good) return PSA_ERROR_GENERIC_ERROR;
    *ciphertext_length = size_t(used + tail) + tag_size;
    return PSA_SUCCESS;
}

psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key_id, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data, size_t additional_data_length,
                              const uint8_t *ciphertext, size_t ciphertext_length, uint8_t *plaintext,
                              size_t plaintext_size, size_t *plaintext_length) {
    std::lock_guard<std::mutex> guard(lock);
    if (!initialized) return PSA_ERROR_BAD_STATE;
    const Key &value = key(key_id, "psa_aead_decrypt");
    if (const psa_status_t status = check_aead(value, alg, PSA_KEY_USAGE_DECRYPT, nonce_length)) return status;
    if (ciphertext_length < tag_size) return PSA_ERROR_INVALID_SIGNATURE;
    if (plaintext_size < ciphertext_length - tag_size) return PSA_ERROR_BUFFER_TOO_SMALL;
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int used = 0, tail = 0;
    const bool good =
        context && EVP_DecryptInit_ex(context, gcm(value.secret.size()), nullptr, value.secret.data(), nonce) > 0 &&
        EVP_DecryptUpdate(context, nullptr, &used, additional_data, int(additional_data_length)) > 0 &&
        EVP_DecryptUpdate(context, plaintext, &used, ciphertext, int(ciphertext_length - tag_size)) > 0 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, int(tag_size),
                            const_cast<uint8_t *>(ciphertext + ciphertext_length - tag_size)) > 0 &&
        EVP_DecryptFinal_ex(context, plaintext + used, &tail) > 0;
    EVP_CIPHER_CTX_free(context);
    if (!good) return PSA_ERROR_INVALID_SIGNATURE;
    *plaintext_length = size_t(used + tail);
    return PSA_SUCCESS;
}

}  // extern "C"
