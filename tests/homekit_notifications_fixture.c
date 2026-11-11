/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
 * The runner inserts production queue and notification functions below.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json_generator.h>
#include <cJSON.h>

#define HAP_SUCCESS 0
#define HAP_FAIL (-1)
#define pdTRUE 1
#define HAP_INTERNAL_EVENT_TRIGGER_NOTIF 1
#define HAP_MAX_SESSIONS 2
#define STATE_VERIFIED 1
#define STATE_INVALID 0
#define ESP_MFI_DEBUG(...) ((void)0)
#define ESP_MFI_DEBUG_PLAIN(...) ((void)0)
typedef enum {
    HAP_CHAR_FORMAT_BOOL, HAP_CHAR_FORMAT_UINT8, HAP_CHAR_FORMAT_UINT16,
    HAP_CHAR_FORMAT_UINT32, HAP_CHAR_FORMAT_INT, HAP_CHAR_FORMAT_FLOAT,
    HAP_CHAR_FORMAT_STRING, HAP_CHAR_FORMAT_DATA, HAP_CHAR_FORMAT_TLV8
} hap_char_format_t;
typedef union {
    bool b; int i; float f; char *s;
    struct { uint8_t *buf; uint32_t buflen; } d;
} hap_val_t;
typedef struct { int aid; } __hap_acc_t;
typedef struct { __hap_acc_t *parent; } hap_serv_t;
typedef struct {
    int iid;
    hap_char_format_t format;
    hap_val_t val;
    uint16_t ev_ctrls, owner_ctrl;
    hap_serv_t *parent;
} __hap_char_t;
typedef __hap_char_t hap_char_t;
typedef struct { int state, conn_identifier; } hap_secure_session_t;
static struct {
    struct { int max_event_notif_chars; } cfg;
    int server;
    hap_secure_session_t *sessions[HAP_MAX_SESSIONS];
    bool disconnected_event_sent;
} hap_priv;
typedef struct { hap_char_t *items[16]; size_t count, capacity; } queue_t;
typedef queue_t *QueueHandle_t;
static queue_t queue;
static QueueHandle_t hap_event_queue;
static bool in_isr;
static unsigned events, announcements, closes[HAP_MAX_SESSIONS], lru[HAP_MAX_SESSIONS];
static struct { char bytes[131072]; size_t size, limit, fail_after; int failure; unsigned calls; } sockets[HAP_MAX_SESSIONS];
static __hap_acc_t accessory = {.aid = 1};
static hap_serv_t service = {.parent = &accessory};
static hap_secure_session_t sessions[HAP_MAX_SESSIONS];
static hap_char_t characteristics[32];

static QueueHandle_t xQueueCreate(unsigned capacity, size_t bytes) {
    assert(capacity == 16 && bytes == sizeof(hap_char_t *));
    queue = (queue_t){.capacity = capacity};
    return &queue;
}
static void vQueueDelete(QueueHandle_t q) { assert(q == &queue); queue.count = 0; }
static int xQueueSend(QueueHandle_t q, const void *item, unsigned wait) {
    assert(q == &queue && !wait);
    if (q->count == q->capacity) return 0;
    q->items[q->count++] = *(hap_char_t *const *)item;
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, void *item, unsigned wait) {
    assert(q == &queue && !wait);
    if (!q->count) return 0;
    *(hap_char_t **)item = q->items[0];
    memmove(q->items, q->items + 1, --q->count * sizeof(q->items[0]));
    return pdTRUE;
}
static int xQueueSendFromISR(QueueHandle_t q, const void *item, void *woken) {
    assert(in_isr && !woken); return xQueueSend(q, item, 0);
}
static int xQueueReceiveFromISR(QueueHandle_t q, void *item, void *woken) {
    assert(in_isr && !woken); return xQueueReceive(q, item, 0);
}
static int xPortInIsrContext(void) { return in_isr; }
static int hap_send_event(int event) { assert(event == HAP_INTERNAL_EVENT_TRIGGER_NOTIF); ++events; return HAP_SUCCESS; }
static hap_serv_t *hap_char_get_parent(hap_char_t *hc) { return hc->parent; }
static __hap_acc_t *hap_serv_get_parent(hap_serv_t *s) { return s->parent; }
static int hap_httpd_send(int server, int fd, const char *bytes, size_t size, int flags) {
    assert(!server && fd >= 0 && fd < HAP_MAX_SESSIONS && !flags);
    ++sockets[fd].calls;
    if (sockets[fd].size >= sockets[fd].fail_after) return sockets[fd].failure;
    if (size > sockets[fd].fail_after - sockets[fd].size) size = sockets[fd].fail_after - sockets[fd].size;
    if (size > sockets[fd].limit) size = sockets[fd].limit;
    assert(size && sockets[fd].size + size < sizeof(sockets[fd].bytes));
    memcpy(sockets[fd].bytes + sockets[fd].size, bytes, size);
    sockets[fd].size += size;
    return size;
}
static void hap_close_session(hap_secure_session_t *session) { ++closes[session->conn_identifier]; }
static void httpd_sess_update_lru_counter(int server, int fd) { assert(!server); ++lru[fd]; }
static void hap_mdns_announce(bool value) { assert(!value); ++announcements; }
static int esp_mfi_base64_encode(char *data, int size, char *out, int capacity, int *length) {
    (void)data; (void)size; (void)out; (void)capacity; (void)length;
    assert(!"Data format is not used by these fixtures"); return -1;
}

/* INSERT PRODUCTION FUNCTIONS */

static void reset(void) {
    memset(&hap_priv, 0, sizeof(hap_priv));
    hap_priv.cfg.max_event_notif_chars = TEST_NOTIF_CAPACITY;
    assert(hap_event_queue_init() == HAP_SUCCESS);
    memset(characteristics, 0, sizeof(characteristics));
    memset(sockets, 0, sizeof(sockets));
    memset(closes, 0, sizeof(closes)); memset(lru, 0, sizeof(lru));
    events = announcements = 0; in_isr = false;
    for (int i = 0; i < HAP_MAX_SESSIONS; ++i) {
        sessions[i] = (hap_secure_session_t){.state = STATE_VERIFIED, .conn_identifier = i};
        hap_priv.sessions[i] = &sessions[i];
        sockets[i].limit = SIZE_MAX; sockets[i].fail_after = SIZE_MAX; sockets[i].failure = -1;
    }
    for (int i = 0; i < 32; ++i) characteristics[i] = (hap_char_t){
        .iid = i + 1, .format = HAP_CHAR_FORMAT_INT, .val.i = i,
        .ev_ctrls = 3, .parent = &service,
    };
}

static unsigned verify_events(int fd, hap_char_t **expected, size_t count) {
    const char *input = sockets[fd].bytes;
    const char *end = input + sockets[fd].size;
    size_t seen = 0; unsigned messages = 0;
    while (input < end) {
        assert(strncmp(input, "EVENT/1.0 200 OK\r\n", 18) == 0);
        const char *content = strstr(input, "Content-Length: ");
        const char *body = strstr(input, "\r\n\r\n");
        assert(content && body && content < body);
        size_t length = strtoul(content + strlen("Content-Length: "), NULL, 10); body += 4;
        assert(length && length <= (size_t)(end - body));
        const char *parse_end = NULL;
        cJSON *root = cJSON_ParseWithLengthOpts(body, length, &parse_end, false);
        assert(root && parse_end == body + length);
        cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "characteristics");
        assert(cJSON_IsArray(array) && cJSON_GetArraySize(array) > 0);
        for (cJSON *item = array->child; item; item = item->next) {
            assert(seen < count);
            hap_char_t *hc = expected[seen++];
            assert(cJSON_GetObjectItemCaseSensitive(item, "aid")->valueint == 1);
            assert(cJSON_GetObjectItemCaseSensitive(item, "iid")->valueint == hc->iid);
            cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "value");
            if (hc->format == HAP_CHAR_FORMAT_STRING) assert(cJSON_IsString(value) && strcmp(value->valuestring, hc->val.s) == 0);
            else assert(cJSON_IsNumber(value) && value->valueint == hc->val.i);
        }
        cJSON_Delete(root); ++messages; input = body + length;
    }
    assert(seen == count && input == end);
    return messages;
}

static void queue_overflow_test(bool isr) {
    reset(); in_isr = isr;
    for (int i = 0; i < 20; ++i) {
        characteristics[i].owner_ctrl = 1;
        assert(hap_queue_event(&characteristics[i]) == HAP_SUCCESS);
        assert(queue.count <= 16);
    }
    assert(events == 20 && queue.count == 16);
    for (int i = 0; i < 4; ++i) assert(!characteristics[i].owner_ctrl);
    for (int i = 4; i < 20; ++i) assert(hap_get_pending_notif_char() == &characteristics[i]);
    assert(!hap_get_pending_notif_char());
    assert(hap_event_queue_deinit() == HAP_SUCCESS && !hap_event_queue);
    assert(hap_queue_event(&characteristics[0]) == HAP_FAIL);
}

static void long_names_test(void) {
    reset();
    char names[16][64]; hap_char_t *expected[16];
    for (int i = 0; i < 16; ++i) {
        memset(names[i], 'a' + i, 63); names[i][63] = '\0';
        characteristics[i].format = HAP_CHAR_FORMAT_STRING; characteristics[i].val.s = names[i];
        expected[i] = &characteristics[i]; assert(hap_queue_event(expected[i]) == HAP_SUCCESS);
    }
    sockets[0].limit = 7; sockets[1].limit = 31; /* Positive short writes. */
    hap_send_notification_unlocked(NULL);
    assert(!queue.count && !closes[0] && !closes[1] && lru[0] == 1 && lru[1] == 1);
    assert(verify_events(0, expected, 16) >= 2 && verify_events(1, expected, 16) >= 2);
}

static void subscriptions_and_failures_test(void) {
    reset();
    characteristics[0].owner_ctrl = 1; characteristics[1].ev_ctrls = 2;
    hap_char_t *expected[16];
    for (int i = 0; i < 16; ++i) { expected[i] = &characteristics[i]; assert(hap_queue_event(expected[i]) == HAP_SUCCESS); }
    hap_send_notification_unlocked(NULL);
    verify_events(0, expected + 2, 14); verify_events(1, expected, 16);
    assert(!characteristics[0].owner_ctrl);
    /* Duplicate entries still skip their original owner for the whole batch. */
    reset(); characteristics[0].owner_ctrl = 1;
    hap_char_t *duplicates[] = {&characteristics[0], &characteristics[0]};
    assert(hap_queue_event(duplicates[0]) == HAP_SUCCESS && hap_queue_event(duplicates[1]) == HAP_SUCCESS);
    hap_send_notification_unlocked(NULL); assert(!sockets[0].size); verify_events(1, duplicates, 2);
    for (int failure = -1; failure <= 0; ++failure) {
        reset(); sockets[0].fail_after = 115; sockets[0].failure = failure;
        for (int i = 0; i < 16; ++i) { expected[i] = &characteristics[i]; assert(hap_queue_event(expected[i]) == HAP_SUCCESS); }
        hap_send_notification_unlocked(NULL);
        assert(closes[0] == 1 && sessions[0].state == STATE_INVALID && !lru[0] && !closes[1]);
        verify_events(1, expected, 16);
    }
}

static void oversized_and_boundary_test(void) {
    char value[4097];
    for (int i = 0; i < 4096; ++i) value[i] = i % 4 == 0 ? '"' : i % 4 == 1 ? '\\' : i % 4 == 2 ? '\n' : 'x';
    value[4096] = '\0';
    reset(); characteristics[0].format = HAP_CHAR_FORMAT_STRING; characteristics[0].val.s = value;
    hap_char_t *expected[] = {&characteristics[0], &characteristics[1]};
    sockets[0].limit = 11;
    assert(hap_queue_event(expected[0]) == HAP_SUCCESS && hap_queue_event(expected[1]) == HAP_SUCCESS);
    hap_send_notification_unlocked(NULL);
    assert(verify_events(0, expected, 2) == 2 && verify_events(1, expected, 2) == 2);
    /* Fill every edge around the fixed buffer's final comma/closing bytes. */
    for (size_t size = 900; size < 1025; ++size) {
        reset(); memset(value, 'x', size); value[size] = '\0';
        characteristics[0].format = HAP_CHAR_FORMAT_STRING; characteristics[0].val.s = value;
        assert(hap_queue_event(expected[0]) == HAP_SUCCESS && hap_queue_event(expected[1]) == HAP_SUCCESS);
        hap_send_notification_unlocked(NULL); verify_events(0, expected, 2); verify_events(1, expected, 2);
    }
}

static void disconnected_test(void) {
    reset(); hap_priv.sessions[0] = hap_priv.sessions[1] = NULL;
    assert(hap_queue_event(&characteristics[0]) == HAP_SUCCESS); hap_send_notification_unlocked(NULL);
    assert(announcements == 1 && hap_priv.disconnected_event_sent);
    assert(hap_queue_event(&characteristics[0]) == HAP_SUCCESS); hap_send_notification_unlocked(NULL);
    assert(announcements == 1);
}

int main(void) {
    queue_overflow_test(false); queue_overflow_test(true); long_names_test();
    subscriptions_and_failures_test(); oversized_and_boundary_test(); disconnected_test();
    puts("HomeKit notifications: 16-slot rolling FIFO, long/escaped JSON batches, oversized streaming, subscriptions and short/failed writes passed");
    return 0;
}
