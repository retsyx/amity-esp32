/* Copyright 2026. Licensed under GPL-3.0-or-later.
 * The Python runner inserts actual production functions at the marker below.
 * Stubs replace SDK, crypto, storage and socket operations only.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESP_OK 0
#define ESP_FAIL (-1)
#define HAP_SUCCESS 0
#define HAP_FAIL (-1)
#define STATE_M0 0
#define STATE_M1 1
#define STATE_M2 2
#define STATE_M3 3
#define STATE_M4 4
#define STATE_M5 5
#define STATE_VERIFIED 0x55
#define PROCESS_PAIR_SETUP 0x33
#define PROCESS_PAIR_VERIFY 0xcc
#define PS_CTX_INIT 1
#define PS_CTX_DEINIT 2
#define kTLVType_State 6
#define kTLVError_Unknown 1
#define kTLVError_Busy 7
#define kTLVError_Unavailable 6
#define ESP_MFI_DEBUG(...) ((void)0)
#define ESP_MFI_DEBUG_PLAIN(...) ((void)0)

typedef int esp_err_t;
typedef void (*free_context_fn)(void *);
typedef struct { bool valid; } controller_t;
typedef struct {
    uint8_t state;
    bool transient;
    int conn_identifier;
    controller_t *ctrl;
} hap_secure_session_t;
typedef struct {
    uint8_t state;
    uint8_t process;
    int sock_fd;
    uint32_t pairing_window;
    hap_secure_session_t *session;
} pair_setup_ctx_t;
typedef struct httpd_req {
    void *sess_ctx;
    free_context_fn free_ctx;
    void *user_ctx;
    size_t content_len;
    const char *network_data;
    const char *status;
    const char *response_type;
    bool connection_close;
} httpd_req_t;
typedef struct {
    const char *uri;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;

static int lock_depth, reads, body_allocs, body_frees, handlers;
static int initializations, cleanups, secure_cleanups, verify_cleanups, responses;
static int send_overrides, recv_overrides;
static bool pairing_allowed = true, fail_context_allocation, fail_body_allocation;
static bool fail_body_read, revoke_while_reading, secure_transition;
static uint32_t pairing_window = 1;
static pair_setup_ctx_t *active_setup;
static void *last_allocation;
static struct { int server; unsigned pair_attempts; } hap_priv;

static esp_err_t hap_http_pair_setup_handler(httpd_req_t *req);
static esp_err_t buffered_handler(httpd_req_t *req);
static int hap_httpd_get_data(httpd_req_t *req, char *buffer, int len);
static httpd_uri_t hap_pair_setup = {"/pair-setup", hap_http_pair_setup_handler};
static httpd_uri_t hap_pair_verify = {"/pair-verify", buffered_handler};
static httpd_uri_t hap_pairings = {"/pairings", buffered_handler};
static httpd_uri_t hap_accessories = {"/accessories", buffered_handler};
static httpd_uri_t hap_characteristics_get = {"/characteristics", buffered_handler};
static httpd_uri_t hap_characteristics_put = {"/characteristics", buffered_handler};
static httpd_uri_t hap_prepare = {"/prepare", buffered_handler};
static httpd_uri_t hap_identify = {"/identify", buffered_handler};

static void hap_amity_lock(void) { ++lock_depth; }
static void hap_amity_unlock(void) { assert(lock_depth > 0); --lock_depth; }
static bool hap_amity_pairing_allowed(void) { return pairing_allowed; }
static uint32_t hap_amity_pairing_window(void) { return pairing_window; }
static bool hap_is_req_secure(hap_secure_session_t *session) {
    return session && !session->transient && session->state == STATE_VERIFIED;
}
static void *hap_platform_httpd_get_sess_ctx(httpd_req_t *req) { return req->sess_ctx; }
static int hap_platform_httpd_set_sess_ctx(httpd_req_t *req, void *ctx, free_context_fn cleanup, bool ignore) {
    assert(ignore);
    req->sess_ctx = ctx;
    req->free_ctx = cleanup;
    return ESP_OK;
}
static int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 17; }
static int httpd_resp_set_status(httpd_req_t *req, const char *status) { req->status = status; return ESP_OK; }
static int httpd_resp_set_type(httpd_req_t *req, const char *type) { req->response_type = type; return ESP_OK; }
static int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value) {
    assert(strcmp(key, "Connection") == 0 && strcmp(value, "close") == 0);
    req->connection_close = true;
    return ESP_OK;
}
static int httpd_resp_send(httpd_req_t *req, const char *data, int length) {
    (void)req;
    assert(length >= 0 && (!length || data));
    ++responses;
    return ESP_OK;
}
static int hap_httpd_send(void) { return ESP_OK; }
static int hap_httpd_recv(void) { return ESP_OK; }
static int httpd_sess_set_send_override(int server, int fd, int (*fn)(void)) {
    (void)server;
    assert(fd == 17 && fn == hap_httpd_send);
    ++send_overrides;
    return ESP_OK;
}
static int httpd_sess_set_recv_override(int server, int fd, int (*fn)(void)) {
    (void)server;
    assert(fd == 17 && fn == hap_httpd_recv);
    ++recv_overrides;
    return ESP_OK;
}
static void *hap_platform_memory_malloc(size_t size) {
    assert(!lock_depth);
    if (fail_body_allocation) return NULL;
    ++body_allocs;
    return malloc(size);
}
static void hap_platform_memory_free(void *data) {
    if (data) ++body_frees;
    free(data);
}
static int hap_http_read_body(httpd_req_t *req, char *data, size_t size) {
    assert(!lock_depth);
    assert(size == req->content_len && data);
    ++reads;
    if (revoke_while_reading) ((hap_secure_session_t *)req->sess_ctx)->ctrl->valid = false;
    if (fail_body_read) return ESP_FAIL;
    memcpy(data, req->network_data, size);
    return ESP_OK;
}
static void hap_prepare_error_tlv(uint8_t state, uint8_t error, void *buffer, int size, int *outlen) {
    assert(size >= 6);
    uint8_t response[6] = {6, 1, state, 7, 1, error};
    memcpy(buffer, response, sizeof(response));
    *outlen = sizeof(response);
}
static int get_value_from_tlv(uint8_t *buffer, int size, uint8_t type, void *value, int capacity) {
    if (size != 3 || buffer[0] != type || buffer[1] != 1 || capacity != 1) return -1;
    *(uint8_t *)value = buffer[2];
    return 1;
}
static pair_setup_ctx_t *hap_pair_setup_ctx_action(int action) {
    if (action == PS_CTX_INIT) {
        if (active_setup || fail_context_allocation) return NULL;
        active_setup = calloc(1, sizeof(*active_setup));
        assert(active_setup);
        ++initializations;
        active_setup->process = PROCESS_PAIR_SETUP;
        last_allocation = active_setup;
    } else {
        assert(action == PS_CTX_DEINIT && active_setup);
        free(active_setup);
        active_setup = NULL;
        ++cleanups;
    }
    return active_setup;
}
static void hap_pair_verify_context_deinit(void *ctx) { ++verify_cleanups; free(ctx); }
static uint8_t hap_pair_verify_get_state(void *ctx) { return ctx ? *(uint8_t *)ctx : 0; }
static void hap_free_session(void *ctx) { ++secure_cleanups; free(ctx); }
static int hap_pair_setup_process_srp_start(pair_setup_ctx_t *ctx, uint8_t *buf, int size,
                                           int capacity, int *outlen) {
    (void)size;
    assert(capacity >= 3);
    ctx->state = STATE_M2;
    buf[0] = 6; buf[1] = 1; buf[2] = STATE_M2;
    *outlen = 3;
    return HAP_SUCCESS;
}
static int hap_pair_setup_process_srp_verify(pair_setup_ctx_t *ctx, uint8_t *buf, int size,
                                            int capacity, int *outlen) {
    (void)size;
    assert(capacity >= 3);
    ctx->state = STATE_M4;
    if (secure_transition) {
        ctx->session = calloc(1, sizeof(*ctx->session));
        assert(ctx->session);
        ctx->session->state = STATE_VERIFIED;
    }
    buf[0] = 6; buf[1] = 1; buf[2] = STATE_M4;
    *outlen = 3;
    return HAP_SUCCESS;
}
static int hap_pair_setup_process_exchange(pair_setup_ctx_t *ctx, uint8_t *buf, int size,
                                          int capacity, int *outlen) {
    (void)ctx; (void)size;
    assert(capacity >= 3);
    buf[0] = 6; buf[1] = 1; buf[2] = 6;
    *outlen = 3;
    return HAP_SUCCESS;
}

/* INSERT PRODUCTION FUNCTIONS */

static esp_err_t buffered_handler(httpd_req_t *req) {
    assert(lock_depth == 1);
    ++handlers;
    char buffer[32768];
    const int first = (int)req->content_len / 2;
    assert(hap_httpd_get_data(req, buffer, first) == first);
    const int second = (int)req->content_len - first;
    assert(hap_httpd_get_data(req, buffer + first, second) == second);
    assert(!req->content_len || memcmp(buffer, req->network_data, req->content_len) == 0);
    assert(hap_httpd_get_data(req, buffer, 1) == -1);
    assert(hap_httpd_get_data(req, buffer, -1) == -1);
    return ESP_OK;
}

static void reset(void) {
    assert(!active_setup && !lock_depth && body_allocs == body_frees);
    reads = body_allocs = body_frees = handlers = initializations = cleanups = 0;
    secure_cleanups = verify_cleanups = responses = send_overrides = recv_overrides = 0;
    pairing_allowed = true;
    fail_context_allocation = fail_body_allocation = fail_body_read = false;
    revoke_while_reading = secure_transition = false;
    hap_priv.pair_attempts = 0;
    last_allocation = NULL;
}

static httpd_req_t request(httpd_uri_t *endpoint, const char *data, size_t size, void *session) {
    httpd_req_t req = {.sess_ctx = session, .user_ctx = endpoint, .content_len = size, .network_data = data};
    return req;
}

static void cleanup_request(httpd_req_t *req) {
    if (req->sess_ctx) {
        assert(req->free_ctx);
        req->free_ctx(req->sess_ctx);
        req->sess_ctx = NULL;
        req->free_ctx = NULL;
    }
}

static void setup_restart_test(void) {
    reset();
    const char m1[] = {6, 1, STATE_M1};
    httpd_req_t req = request(&hap_pair_setup, m1, sizeof(m1), NULL);
    assert(hap_locked_request(&req) == ESP_OK);
    assert(req.sess_ctx == active_setup && req.free_ctx == hap_pair_setup_ctx_clean);
    assert(active_setup->state == STATE_M2 && active_setup->sock_fd == 17);
    void *old = req.sess_ctx;
    assert(hap_locked_request(&req) == ESP_OK);
    assert(req.sess_ctx == active_setup && req.sess_ctx == last_allocation);
    /* ASan's quarantine forces a different address, exposing the original UAF. */
    assert(req.sess_ctx != old && cleanups == 1);
    assert(req.free_ctx == hap_pair_setup_ctx_clean);
    assert(req.user_ctx == &hap_pair_setup);
    cleanup_request(&req);
    assert(cleanups == 2 && !active_setup && !lock_depth);
}

static void failed_restart_test(void) {
    reset();
    const char m1[] = {6, 1, STATE_M1};
    httpd_req_t req = request(&hap_pair_setup, m1, sizeof(m1), NULL);
    assert(hap_locked_request(&req) == ESP_OK);
    fail_context_allocation = true;
    assert(hap_locked_request(&req) == ESP_OK);
    assert(!req.sess_ctx && !req.free_ctx && !active_setup && cleanups == 1);
    assert(req.user_ctx == &hap_pair_setup && !lock_depth);
}

static void failed_restart_process_pointer_test(void) {
    reset();
    uint8_t buffer[16] = {6, 1, STATE_M1};
    void *ctx = NULL;
    int outlen = 0;
    assert(hap_pair_setup_context_init(17, &ctx, buffer, sizeof(buffer), &outlen) == HAP_SUCCESS);
    assert(hap_pair_setup_process(&ctx, buffer, 3, sizeof(buffer), &outlen) == HAP_SUCCESS);
    buffer[2] = STATE_M1;
    fail_context_allocation = true;
    assert(hap_pair_setup_process(&ctx, buffer, 3, sizeof(buffer), &outlen) == HAP_FAIL);
    assert(!ctx && !active_setup && cleanups == 1 && initializations == 1);
}

static void setup_completion_test(bool verified) {
    reset();
    const char m1[] = {6, 1, STATE_M1};
    const char m3[] = {6, 1, STATE_M3};
    const char m5[] = {6, 1, STATE_M5};
    httpd_req_t req = request(&hap_pair_setup, m1, sizeof(m1), NULL);
    assert(hap_locked_request(&req) == ESP_OK);
    secure_transition = verified;
    req.network_data = m3;
    assert(hap_locked_request(&req) == ESP_OK);
    if (verified) {
        assert(!active_setup && cleanups == 1);
        assert(req.sess_ctx && req.free_ctx == hap_free_session);
        assert(((hap_secure_session_t *)req.sess_ctx)->conn_identifier == 17);
        assert(send_overrides == 1 && recv_overrides == 1);
        cleanup_request(&req);
        assert(secure_cleanups == 1);
    } else {
        assert(active_setup && active_setup->state == STATE_M4);
        req.network_data = m5;
        assert(hap_locked_request(&req) == ESP_OK);
        assert(!active_setup && !req.sess_ctx && !req.free_ctx && cleanups == 1);
    }
}

static void wrapper_tests(void) {
    static const char content[32769] = "test body";
    controller_t controller = {.valid = true};
    hap_secure_session_t session = {.state = STATE_VERIFIED, .ctrl = &controller};
    httpd_uri_t *endpoints[] = {&hap_pair_setup, &hap_pair_verify, &hap_pairings,
        &hap_accessories, &hap_characteristics_get, &hap_characteristics_put, &hap_prepare, &hap_identify};
    const size_t limits[] = {1200, 512, 2048, 0, 0, 32768, 512, 0};
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        reset();
        httpd_req_t req = request(endpoints[i], content, limits[i] + 1, &session);
        assert(hap_locked_request(&req) == ESP_FAIL);
        assert(strcmp(req.status, "413 Content Too Large") == 0 && req.connection_close);
        assert(!reads && !body_allocs && !handlers && !active_setup && !lock_depth);
        assert(req.user_ctx == endpoints[i]);
    }
    httpd_uri_t *protected[] = {&hap_pairings, &hap_accessories, &hap_characteristics_get,
                               &hap_characteristics_put, &hap_prepare};
    for (size_t i = 0; i < sizeof(protected) / sizeof(protected[0]); ++i) {
        reset();
        httpd_req_t req = request(protected[i], content, 0, NULL);
        assert(hap_locked_request(&req) == ESP_FAIL);
        assert(strcmp(req.status, "470 Connection Authorization Required") == 0);
        assert(req.connection_close && !reads && !body_allocs && !handlers && !lock_depth);
    }
    reset();
    pairing_allowed = false;
    httpd_req_t req = request(&hap_pair_setup, content, 3, NULL);
    assert(hap_locked_request(&req) == ESP_FAIL);
    assert(req.connection_close && !reads && !body_allocs && !active_setup && !lock_depth);

    for (size_t i = 0; i < sizeof(protected) / sizeof(protected[0]); ++i) {
        reset();
        const size_t size = protected[i] == &hap_accessories || protected[i] == &hap_characteristics_get ? 0 : 9;
        req = request(protected[i], content, size, &session);
        assert(hap_locked_request(&req) == ESP_OK);
        assert(handlers == 1 && reads == (size ? 1 : 0) && body_allocs == body_frees);
        assert(req.user_ctx == protected[i] && !lock_depth);
    }
    for (size_t i = 1; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        reset();
        req = request(endpoints[i], content, limits[i], &session);
        assert(hap_locked_request(&req) == ESP_OK);
        assert(handlers == 1 && reads == (limits[i] ? 1 : 0));
        assert(body_allocs == body_frees && req.user_ctx == endpoints[i] && !lock_depth);
    }
    reset();
    revoke_while_reading = true;
    req = request(&hap_characteristics_put, content, 9, &session);
    assert(hap_locked_request(&req) == ESP_FAIL);
    assert(reads == 1 && !handlers && !controller.valid);
    assert(body_allocs == body_frees && req.user_ctx == &hap_characteristics_put && !lock_depth);
    controller.valid = true;

    for (int allocation = 0; allocation < 2; ++allocation) {
        reset();
        fail_body_allocation = allocation != 0;
        fail_body_read = allocation == 0;
        req = request(&hap_pair_setup, content, 3, NULL);
        assert(hap_locked_request(&req) == ESP_FAIL);
        assert(!req.sess_ctx && !active_setup && !handlers && !initializations);
        assert(reads == (allocation ? 0 : 1) && body_allocs == body_frees);
        assert(req.user_ctx == &hap_pair_setup && !lock_depth);
    }
}

int main(void) {
    setup_restart_test();
    failed_restart_test();
    failed_restart_process_pointer_test();
    setup_completion_test(false);
    setup_completion_test(true);
    wrapper_tests();
    reset();
    puts("HomeKit HTTP context replacement, cleanup, body buffering and authorization tests passed");
    return 0;
}
