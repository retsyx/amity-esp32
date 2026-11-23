/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The esp-tls types and constants Amity's HTTPS server code uses, from the
// pinned ESP-IDF (sdk_parity/web_server.def). Sessions are opaque, as in the
// SDK's public header.
#include "esp_err.h"
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_TLS_VER_ANY = 0,
    ESP_TLS_VER_TLS_1_2 = 0x1,
    ESP_TLS_VER_TLS_1_3 = 0x2,
    ESP_TLS_VER_TLS_MAX,
} esp_tls_proto_ver_t;
// The ESP32-S3 has no P-384 ECDSA support.
typedef enum {
    ESP_TLS_ECDSA_CURVE_SECP256R1 = 0,
    ESP_TLS_ECDSA_CURVE_MAX,
} esp_tls_ecdsa_curve_t;

typedef struct esp_tls esp_tls_t;
// Complete, so a test can own one, but opaque: Amity only passes it on.
typedef struct esp_tls_cfg_server {
    void *private_opaque[32];
} esp_tls_cfg_server_t;

#ifdef __cplusplus
}
#endif
