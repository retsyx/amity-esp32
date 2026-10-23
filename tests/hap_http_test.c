/* Security regression coverage for the production HAP body reader. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "amity_hap_http.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

enum scenario { NORMAL, INCOMPLETE, DRIP, NESTED_FRAME, CREATE_FAILURE, START_FAILURE, DEADLINE_EDGE, STOP_RACE };
struct fake_timer {
    esp_timer_create_args_t args;
    int64_t deadline;
    bool armed;
    bool stopped;
};
static struct fake_timer watchdog;
static enum scenario scenario;
static int64_t now;
static unsigned reads, shutdowns, creates, starts, stops, deletes;
static size_t input_offset;
static bool socket_shutdown;
static const char input[] = "HomeKit";

static void advance(int64_t micros)
{
    now += micros;
    if (watchdog.armed && now >= watchdog.deadline) {
        watchdog.armed = false;
        watchdog.args.callback(watchdog.args.arg);
    }
}

int shutdown(int fd, int how)
{
    assert(fd == 42 && how == SHUT_RDWR);
    socket_shutdown = true;
    ++shutdowns;
    return 0;
}

int httpd_req_to_sockfd(httpd_req_t *req) { return req->fd; }
int64_t esp_timer_get_time(void) { return now; }

int httpd_req_recv(httpd_req_t *req, char *buffer, size_t size)
{
    assert(req->fd == 42);
    ++reads;
    if (scenario == NESTED_FRAME) {
        /* An encrypted record can keep reading internally without yielding to
         * the body loop. Only the watchdog's socket shutdown interrupts it.
         */
        while (!socket_shutdown) advance(4000000);
        return -1;
    }
    if (scenario == INCOMPLETE && input_offset == 2) return 0;
    if (scenario == DRIP) advance(4000000);
    else if (scenario == DEADLINE_EDGE) advance(HAP_HTTP_BODY_TIMEOUT_US);
    else if (scenario == STOP_RACE) advance(HAP_HTTP_BODY_TIMEOUT_US - 1000);
    else advance(1000);
    if (socket_shutdown) return -1;
    size_t count = sizeof(input) - 1 - input_offset;
    if (scenario != STOP_RACE && count > 2) count = 2;
    if (count > size) count = size;
    memcpy(buffer, input + input_offset, count);
    input_offset += count;
    return (int)count;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    ++creates;
    if (scenario == CREATE_FAILURE) return ESP_FAIL;
    assert(args->dispatch_method == ESP_TIMER_TASK);
    watchdog.args = *args;
    *timer = &watchdog;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t micros)
{
    assert(timer == &watchdog && micros == HAP_HTTP_BODY_TIMEOUT_US);
    ++starts;
    if (scenario == START_FAILURE) return ESP_FAIL;
    watchdog.deadline = now + (int64_t)micros;
    watchdog.armed = true;
    return ESP_OK;
}
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t ticks)
{
    assert(timer == &watchdog && ticks == portMAX_DELAY);
    ++stops;
    if (scenario == STOP_RACE) advance(1000);
    timer->armed = false;
    timer->stopped = true;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer == &watchdog && !timer->armed);
    assert(timer->stopped || scenario == START_FAILURE);
    ++deletes;
    return ESP_OK;
}

static void reset(enum scenario next)
{
    memset(&watchdog, 0, sizeof(watchdog));
    scenario = next;
    now = 1234567;
    reads = shutdowns = creates = starts = stops = deletes = 0;
    input_offset = 0;
    socket_shutdown = false;
}

int main(void)
{
    httpd_req_t request = {.content_len = sizeof(input) - 1, .fd = 42};
    char output[sizeof(input)] = {0};
    reset(NORMAL);
    assert(hap_http_read_body(&request, output, request.content_len) == ESP_OK);
    assert(strcmp(output, input) == 0 && reads == 4 && shutdowns == 0);
    assert(stops == 1 && deletes == 1);
    /* A finished request cannot leave a watchdog targeting a reused socket. */
    advance(HAP_HTTP_BODY_TIMEOUT_US * 2);
    assert(shutdowns == 0);

    const enum scenario failures[] = {INCOMPLETE, DRIP, NESTED_FRAME, CREATE_FAILURE, START_FAILURE, DEADLINE_EDGE, STOP_RACE};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset(failures[i]);
        assert(hap_http_read_body(&request, output, request.content_len) == ESP_FAIL);
        assert(socket_shutdown && shutdowns >= 1);
        assert(stops == (scenario != CREATE_FAILURE && scenario != START_FAILURE));
        assert(deletes == (scenario != CREATE_FAILURE));
        if (scenario == DRIP || scenario == NESTED_FRAME)
            assert(now < (int64_t)HAP_HTTP_BODY_TIMEOUT_US + 5000000);
    }
    reset(NORMAL);
    assert(hap_http_read_body(&request, output, request.content_len - 1) == ESP_FAIL);
    assert(creates == 0 && reads == 0 && shutdowns == 1);
    reset(NORMAL);
    assert(hap_http_read_body(&request, NULL, request.content_len) == ESP_FAIL);
    assert(creates == 0 && reads == 0 && shutdowns == 1);
    reset(NORMAL);
    request.content_len = 0;
    assert(hap_http_read_body(&request, NULL, 0) == ESP_OK);
    assert(creates == 0 && reads == 0 && shutdowns == 0);
    assert(hap_http_read_body(NULL, NULL, 0) == ESP_FAIL);
    puts("HAP body-reader security tests passed");
    return 0;
}
