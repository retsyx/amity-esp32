/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 */
#include "amity_hap_http.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static void body_deadline(void *context)
{
    /* Shutdown wakes both ordinary recv and the nested encrypted-frame reader.
     * The HTTP server still owns closing the descriptor and freeing its session.
     */
    shutdown(*(int *)context, SHUT_RDWR);
}

esp_err_t hap_http_read_body(httpd_req_t *req, char *buffer, size_t size)
{
    if (!req) return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;
    if (size != req->content_len || (!buffer && size)) {
        shutdown(fd, SHUT_RDWR);
        return ESP_FAIL;
    }
    if (!size) return ESP_OK;

    /* This context stays alive until stop_blocking has joined any callback.
     * In particular, no delayed watchdog may shut down a reused descriptor.
     */
    int watchdog_fd = fd;
    esp_timer_handle_t timer = NULL;
    const esp_timer_create_args_t args = {
        .callback = body_deadline,
        .arg = &watchdog_fd,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "hap_body",
    };
    if (esp_timer_create(&args, &timer) != ESP_OK) {
        shutdown(fd, SHUT_RDWR);
        return ESP_FAIL;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)HAP_HTTP_BODY_TIMEOUT_US;
    if (esp_timer_start_once(timer, HAP_HTTP_BODY_TIMEOUT_US) != ESP_OK) {
        esp_timer_delete(timer);
        shutdown(fd, SHUT_RDWR);
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < size && esp_timer_get_time() < deadline) {
        const int count = httpd_req_recv(req, buffer + received, size - received);
        if (count <= 0 || (size_t)count > size - received) break;
        received += count;
    }
    const bool complete = received == size;
    /* This valid TASK timer is stopped from the HTTP task. With an unbounded
     * wait the API guarantees no callback can run after returning ESP_OK.
     * Fail fatally on a violated invariant rather than leave a stack callback.
     */
    ESP_ERROR_CHECK(esp_timer_stop_blocking(timer, portMAX_DELAY));
    const esp_err_t deleted = esp_timer_delete(timer);
    if (!complete || esp_timer_get_time() >= deadline || deleted != ESP_OK) {
        shutdown(fd, SHUT_RDWR);
        return ESP_FAIL;
    }
    return ESP_OK;
}
