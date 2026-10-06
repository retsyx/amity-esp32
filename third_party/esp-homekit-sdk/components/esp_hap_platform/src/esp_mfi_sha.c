/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS products only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/* ESP-IDF 6 / Mbed TLS 4 adapter using the public PSA hash API. */
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "psa/crypto.h"
#include "esp_mfi_sha.h"

typedef struct {
    psa_hash_operation_t operation;
} hap_psa_hash_t;

static void hash_check(psa_status_t status)
{
    /* The legacy SDK callbacks cannot propagate crypto failures. Fail closed. */
    if (status != PSA_SUCCESS) {
        abort();
    }
}

static void *hash_new(void)
{
    hash_check(psa_crypto_init());
    hap_psa_hash_t *context = calloc(1, sizeof(*context));
    if (context) {
        context->operation = psa_hash_operation_init();
    }
    return context;
}

static void hash_free(void *opaque)
{
    hap_psa_hash_t *context = opaque;
    if (context) {
        psa_hash_abort(&context->operation);
        free(context);
    }
}

static void hash_start(void *opaque, psa_algorithm_t algorithm)
{
    hap_psa_hash_t *context = opaque;
    if (context) {
        hash_check(psa_hash_abort(&context->operation));
        hash_check(psa_hash_setup(&context->operation, algorithm));
    }
}

static void hash_update(void *opaque, const uint8_t *input, int length)
{
    hap_psa_hash_t *context = opaque;
    if (context && input && length >= 0) {
        hash_check(psa_hash_update(&context->operation, input, (size_t)length));
    }
}

static void hash_finish(void *opaque, uint8_t *digest, size_t size)
{
    hap_psa_hash_t *context = opaque;
    if (context && digest) {
        size_t actual = 0;
        hash_check(psa_hash_finish(&context->operation, digest, size, &actual));
        if (actual != size) {
            abort();
        }
    }
}

#define HAP_PSA_HASH(bits, algorithm, bytes) \
    esp_mfi_sha_ctx_t esp_mfi_sha##bits##_new(void) { return hash_new(); } \
    void esp_mfi_sha##bits##_free(esp_mfi_sha_ctx_t context) { hash_free(context); } \
    void esp_mfi_sha##bits##_init(esp_mfi_sha_ctx_t context) { hash_start(context, algorithm); } \
    void esp_mfi_sha##bits##_update(esp_mfi_sha_ctx_t context, const uint8_t *input, int length) \
        { hash_update(context, input, length); } \
    void esp_mfi_sha##bits##_final(esp_mfi_sha_ctx_t context, uint8_t *digest) \
        { hash_finish(context, digest, bytes); }

HAP_PSA_HASH(1, PSA_ALG_SHA_1, 20)
HAP_PSA_HASH(256, PSA_ALG_SHA_256, 32)
HAP_PSA_HASH(512, PSA_ALG_SHA_512, 64)
