/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// An esp-tls session, from the pinned ESP-IDF's
// esp-tls/private_include/esp_tls_private.h for the Amity build's Mbed TLS
// stack: its TLS context and the fields after the Mbed TLS state, in the
// SDK's order.
#include "esp_tls.h"
#include "mbedtls/ssl.h"
#include <stdbool.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

struct esp_tls {
    mbedtls_ssl_context ssl;
    int sockfd;
    ssize_t (*read)(esp_tls_t *tls, char *data, size_t datalen);
    ssize_t (*write)(esp_tls_t *tls, const char *data, size_t datalen);
    esp_tls_conn_state_t conn_state;
    fd_set rset;
    fd_set wset;
    bool is_tls;
    int role;
    esp_tls_error_handle_t error_handle;
};

#ifdef __cplusplus
}
#endif
