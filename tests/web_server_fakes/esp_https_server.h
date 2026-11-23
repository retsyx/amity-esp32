/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The HTTPS server API, with the pinned ESP-IDF's configuration layout and
// defaults for the Amity build (sdk_parity/web_server.def). A test defines
// httpd_ssl_start and httpd_ssl_stop.
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_tls.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTPD_SSL_TRANSPORT_SECURE,
    HTTPD_SSL_TRANSPORT_INSECURE,
} httpd_ssl_transport_mode_t;
typedef struct esp_key_config esp_key_config_t;
typedef struct esp_https_server_user_cb_arg esp_https_server_user_cb_arg_t;
typedef void esp_https_server_user_cb(esp_https_server_user_cb_arg_t *user_cb);
typedef esp_err_t (*esp_https_server_cert_select_cb)(void *ssl);

// CONFIG_ESP_TLS_SERVER_MIN_AUTH_MODE_OPTIONAL is off in the Amity build.
struct httpd_ssl_config {
    httpd_config_t httpd;
    const uint8_t *servercert;
    size_t servercert_len;
    const uint8_t *cacert_pem;
    size_t cacert_len;
    const uint8_t *prvtkey_pem;
    size_t prvtkey_len;
    const esp_key_config_t *server_key;
    bool use_ecdsa_peripheral;
    uint8_t ecdsa_key_efuse_blk;
    uint8_t ecdsa_key_efuse_blk_high;
    esp_tls_ecdsa_curve_t ecdsa_curve;
    httpd_ssl_transport_mode_t transport_mode;
    uint16_t port_secure;
    uint16_t port_insecure;
    bool session_tickets;
    bool use_secure_element;
    esp_https_server_user_cb *user_cb;
    void *ssl_userdata;
    esp_https_server_cert_select_cb cert_select_cb;
    const char **alpn_protos;
    uint32_t tls_handshake_timeout_ms;
    esp_tls_proto_ver_t tls_version;
    const int *ciphersuites_list;
};
typedef struct httpd_ssl_config httpd_ssl_config_t;

#define HTTPD_SSL_CONFIG_DEFAULT()                                    \
    {                                                                 \
        .httpd =                                                      \
            {                                                         \
                .task_priority = tskIDLE_PRIORITY + 5,                \
                .stack_size = 10240,                                  \
                .core_id = tskNO_AFFINITY,                            \
                .task_caps = (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), \
                .max_req_hdr_len = CONFIG_HTTPD_MAX_REQ_HDR_LEN,      \
                .max_uri_len = CONFIG_HTTPD_MAX_URI_LEN,              \
                .server_port = 0,                                     \
                .ctrl_port = 32768 + 1,                               \
                .max_open_sockets = 4,                                \
                .max_uri_handlers = 8,                                \
                .max_resp_headers = 8,                                \
                .backlog_conn = 5,                                    \
                .lru_purge_enable = true,                             \
                .recv_wait_timeout = 5,                               \
                .send_wait_timeout = 5,                               \
                .global_user_ctx = NULL,                              \
                .global_user_ctx_free_fn = NULL,                      \
                .global_transport_ctx = NULL,                         \
                .global_transport_ctx_free_fn = NULL,                 \
                .enable_so_linger = false,                            \
                .linger_timeout = 0,                                  \
                .keep_alive_enable = false,                           \
                .keep_alive_idle = 0,                                 \
                .keep_alive_interval = 0,                             \
                .keep_alive_count = 0,                                \
                .if_name = NULL,                                      \
                .open_fn = NULL,                                      \
                .close_fn = NULL,                                     \
                .uri_match_fn = NULL,                                 \
            },                                                        \
        .servercert = NULL, .servercert_len = 0, .cacert_pem = NULL, .cacert_len = 0, .prvtkey_pem = NULL, \
        .prvtkey_len = 0, .server_key = NULL, .use_ecdsa_peripheral = false, .ecdsa_key_efuse_blk = 0,     \
        .ecdsa_key_efuse_blk_high = 0, .ecdsa_curve = ESP_TLS_ECDSA_CURVE_SECP256R1,                     \
        .transport_mode = HTTPD_SSL_TRANSPORT_SECURE, .port_secure = 443, .port_insecure = 80,            \
        .session_tickets = false, .use_secure_element = false, .user_cb = NULL, .ssl_userdata = NULL,    \
        .cert_select_cb = NULL, .alpn_protos = NULL, .tls_handshake_timeout_ms = 0,                      \
        .tls_version = ESP_TLS_VER_ANY, .ciphersuites_list = NULL,                                       \
    }

esp_err_t httpd_ssl_start(httpd_handle_t *handle, httpd_ssl_config_t *config);
esp_err_t httpd_ssl_stop(httpd_handle_t handle);

#ifdef __cplusplus
}
#endif
