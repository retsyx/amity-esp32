/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The pinned ESP-IDF functions https_response_write_test compiles as C
// (https_response_write_sdk.json), and the stand-ins the test defines for
// what they call. HTTPD and esp-tls state come from tests/web_server_fakes.
#include "esp_heap_caps.h"
#include "esp_httpd_priv.h"
#include "esp_tls_private.h"
#include "mbedtls/platform_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_aes_context {
    int key_bytes;
} esp_aes_context;

// From the SDK. Amity's wrapper stands in front of esp_tls_conn_write and
// the test in front of httpd_resp_send_chunk, so they carry other names; the
// AES-DMA helper is not static, so the test's TLS records can use it.
int esp_aes_process_dma_ext_ram(esp_aes_context *ctx, const unsigned char *input, unsigned char *output, size_t len,
                                uint8_t *stream_out, bool realloc_input, bool realloc_output);
ssize_t esp_mbedtls_write(esp_tls_t *tls, const char *data, size_t datalen);
ssize_t __real_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen);
esp_err_t sdk_httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);

// Defined by the test: Mbed TLS records, the AES-DMA engine and its allocator.
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len);
int esp_aes_process_dma(esp_aes_context *ctx, const unsigned char *input, unsigned char *output, size_t len,
                        uint8_t *stream_out);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void dma_free(void *memory);

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
// What the SDK functions see in place of their own includes: the Amity
// build's configuration, and logs that print nothing, since some of their
// arguments do not match their formats on a 64-bit host.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define AES_MAX_CHUNK_WRITE_SIZE 1600
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_INT_EVENT_TRACKER_CAPTURE(handle, type, code) ((void)(handle))
static const char *TAG = "sdk";
static void mbedtls_print_error_msg(int error) { (void)error; }
#endif
