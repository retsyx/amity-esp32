/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// HTTPD's private session and request state, from the pinned ESP-IDF's
// esp_http_server/src/esp_httpd_priv.h with the Amity build's configuration:
// the fields up to the last one the HTTPS deadline guard and the sliced
// response path use, in the SDK's order (sdk_parity/web_server.def).
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARSER_BLOCK_SIZE 128

// Public in the SDK's esp_http_server.h.
typedef int (*httpd_send_func_t)(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags);
typedef int (*httpd_recv_func_t)(httpd_handle_t hd, int sockfd, char *buf, size_t buf_len, int flags);
typedef int (*httpd_pending_func_t)(httpd_handle_t hd, int sockfd);
typedef enum {
    HTTP_SERVER_EVENT_ERROR = 0,
    HTTP_SERVER_EVENT_START,
    HTTP_SERVER_EVENT_ON_CONNECTED,
    HTTP_SERVER_EVENT_ON_HEADER,
    HTTP_SERVER_EVENT_HEADERS_SENT,
    HTTP_SERVER_EVENT_ON_DATA,
    HTTP_SERVER_EVENT_SENT_DATA,
    HTTP_SERVER_EVENT_DISCONNECTED,
    HTTP_SERVER_EVENT_STOP,
} esp_http_server_event_id_t;
typedef struct {
    int fd;
    int data_len;
} esp_http_server_event_data;

struct sock_db {
    int fd;
    void *ctx;
    bool ignore_sess_ctx_changes;
    void *transport_ctx;
    httpd_handle_t handle;
    httpd_free_ctx_fn_t free_ctx;
    httpd_free_ctx_fn_t free_transport_ctx;
    httpd_send_func_t send_fn;
    httpd_recv_func_t recv_fn;
    httpd_pending_func_t pending_fn;
    uint64_t lru_counter;
    bool lru_socket;
    char pending_data[PARSER_BLOCK_SIZE];
    size_t pending_len;
    bool for_async_req;
};

struct httpd_req_aux {
    struct sock_db *sd;
    char *scratch;
    size_t scratch_size_limit;
    size_t scratch_cur_size;
    size_t max_req_hdr_len;
    size_t max_uri_len;
    size_t remaining_len;
    char *status;
    char *content_type;
    bool first_chunk_sent;
    unsigned req_hdrs_count;
    unsigned resp_hdrs_count;
    struct resp_hdr {
        const char *field;
        const char *value;
    } *resp_hdrs;
};

struct httpd_data {
    httpd_config_t config;
    int listen_fd;
    int ctrl_fd;
    SemaphoreHandle_t ctrl_sock_semaphore;
    int msg_fd;
    struct {
        TaskHandle_t handle;
        int status;
    } hd_td;
    struct sock_db *hd_sd;
    int hd_sd_active_count;
    httpd_uri_t **hd_calls;
    struct httpd_req hd_req;
    struct httpd_req_aux hd_req_aux;
    uint64_t lru_counter;
    esp_http_server_event_id_t http_server_state;
};

// CONFIG_HTTPD_VALIDATE_REQ is off in the Amity build.
#define httpd_valid_req(r) true
// CONFIG_HTTPD_ENABLE_EVENTS is on in the Amity build.
void esp_http_server_dispatch_event(int32_t event_id, const void *event_data, size_t event_data_size);

#ifdef __cplusplus
}
#endif
