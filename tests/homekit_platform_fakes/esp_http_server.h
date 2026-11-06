#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define tskIDLE_PRIORITY 0
#define MALLOC_CAP_8BIT 4u
#define MALLOC_CAP_SPIRAM 1024u
#define MALLOC_CAP_INTERNAL 2048u
#define CONFIG_HAP_HTTP_STACK_SIZE 12288
#define CONFIG_HAP_HTTP_SERVER_PORT 51826
#define CONFIG_HAP_HTTP_CONTROL_PORT 32859
#define CONFIG_HAP_HTTP_MAX_OPEN_SOCKETS 4
#define CONFIG_HAP_HTTP_MAX_URI_HANDLERS 8
typedef int esp_err_t;
typedef void *httpd_handle_t;
typedef void (*httpd_free_ctx_fn_t)(void *);
typedef struct {
    void *sess_ctx;
    httpd_free_ctx_fn_t free_ctx;
    bool ignore_sess_ctx_changes;
    int content_len;
    int method;
    const char *uri;
} httpd_req_t;
typedef struct {
    int task_priority;
    size_t stack_size;
    uint32_t task_caps;
    int server_port, ctrl_port, max_open_sockets, max_uri_handlers;
    int max_resp_headers, backlog_conn;
    bool lru_purge_enable;
    int recv_wait_timeout, send_wait_timeout;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() { .stack_size = 4096, .task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT }
enum {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE};
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
#ifdef __cplusplus
}
#endif
