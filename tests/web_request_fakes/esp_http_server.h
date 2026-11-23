/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The HTTP server API Amity uses, with the SDK's request and configuration
// layouts (CONFIG_HTTPD_MAX_URI_LEN 256). Requests are amity_fake::HttpRequest
// objects, which script the request and record the response; see httpd_fake.hpp.
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTPD_MAX_URI_LEN 256
#define ESP_ERR_HTTPD_BASE (0xb000)
#define ESP_ERR_HTTPD_HANDLERS_FULL (ESP_ERR_HTTPD_BASE + 1)
#define ESP_ERR_HTTPD_HANDLER_EXISTS (ESP_ERR_HTTPD_BASE + 2)
#define ESP_ERR_HTTPD_INVALID_REQ (ESP_ERR_HTTPD_BASE + 3)
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 4)
#define ESP_ERR_HTTPD_RESP_HDR (ESP_ERR_HTTPD_BASE + 5)
#define ESP_ERR_HTTPD_RESP_SEND (ESP_ERR_HTTPD_BASE + 6)
#define ESP_ERR_HTTPD_ALLOC_MEM (ESP_ERR_HTTPD_BASE + 7)
#define ESP_ERR_HTTPD_TASK (ESP_ERR_HTTPD_BASE + 8)
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3

#define HTTPD_200 "200 OK"
#define HTTPD_204 "204 No Content"
#define HTTPD_400 "400 Bad Request"
#define HTTPD_404 "404 Not Found"
#define HTTPD_500 "500 Internal Server Error"
#define HTTPD_TYPE_JSON "application/json"
#define HTTPD_TYPE_TEXT "text/html"
#define HTTPD_TYPE_OCTET "application/octet-stream"

enum http_method {
    HTTP_DELETE = 0,
    HTTP_GET = 1,
    HTTP_HEAD = 2,
    HTTP_POST = 3,
    HTTP_PUT = 4,
    HTTP_CONNECT = 5,
    HTTP_OPTIONS = 6,
    HTTP_TRACE = 7,
};
#define HTTP_ANY INT_MAX

typedef void *httpd_handle_t;
typedef enum http_method httpd_method_t;
typedef void (*httpd_free_ctx_fn_t)(void *ctx);
typedef esp_err_t (*httpd_open_func_t)(httpd_handle_t hd, int sockfd);
typedef void (*httpd_close_func_t)(httpd_handle_t hd, int sockfd);
typedef bool (*httpd_uri_match_func_t)(const char *reference_uri, const char *uri_to_match, size_t match_upto);

struct ifreq;
typedef struct httpd_config {
    unsigned task_priority;
    size_t stack_size;
    BaseType_t core_id;
    uint32_t task_caps;
    size_t max_req_hdr_len;
    size_t max_uri_len;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    void *global_user_ctx;
    httpd_free_ctx_fn_t global_user_ctx_free_fn;
    void *global_transport_ctx;
    httpd_free_ctx_fn_t global_transport_ctx_free_fn;
    bool enable_so_linger;
    int linger_timeout;
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
    struct ifreq *if_name;
    httpd_open_func_t open_fn;
    httpd_close_func_t close_fn;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char uri[HTTPD_MAX_URI_LEN + 1];
    size_t content_len;
    void *aux;
    void *user_ctx;
    void *sess_ctx;
    httpd_free_ctx_fn_t free_ctx;
    bool ignore_sess_ctx_changes;
} httpd_req_t;

typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

// Recorded per server handle; amity_fake::uri_handlers() lists them.
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
// A strict seam: install a behaviour with amity_seam::install.
bool httpd_uri_match_wildcard(const char *uri_template, const char *uri_to_match, size_t match_upto);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *r);
int httpd_req_to_sockfd(httpd_req_t *r);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
// As in the SDK, these keep the caller's pointers until the response head is sent.
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str) {
    return httpd_resp_send(r, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str) {
    return httpd_resp_send_chunk(r, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}

#ifdef __cplusplus
}
#endif
