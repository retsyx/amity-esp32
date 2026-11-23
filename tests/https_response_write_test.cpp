/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Amity's response path, components/web/request.cpp and tls_write.cpp, built
// whole over the pinned SDK's HTTPD send path and esp-tls write, which
// https_response_write_sdk.json cuts from the SDK. Mbed TLS records and the
// AES-DMA engine stand in below them under a modeled 1024-byte DMA limit;
// this is not a measurement of bench free heap.
#include "web.hpp"
#include "stall.hpp"
#include "core_fakes.hpp"
#include "https_response_write_sdk.h"
#include "scenario_main.hpp"
#include "sdkconfig.h"
#include "seams.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

extern "C" ssize_t __wrap_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t size);

namespace {

// The modeled largest available DMA block. No heap numbers from this test
// describe the physical failure instant.
size_t dma_limit = 1024, largest_dma_request = 0, dma_peak = 0, dma_live = 0;
unsigned dma_failures = 0;
// What reached the socket, after encryption.
std::string wire;
unsigned tls_calls = 0, fail_tls_call = 0, zero_tls_call = 0;
std::vector<size_t> tls_records;
bool short_writes = false, want_next = false;
unsigned chunk_calls = 0, terminators = 0;
// The sizes web::stall::Response bounded.
std::vector<size_t> bounded;
struct Event {
    int32_t id;
    int fd, data_len;
};
std::vector<Event> events;
esp_tls_t tls{};
constexpr int socket_fd = 9;

}  // namespace

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    CHECK(alignment == 1 && caps == MALLOC_CAP_DMA);
    largest_dma_request = std::max(largest_dma_request, size);
    dma_failures += size > dma_limit;
    if (size > dma_limit) return nullptr;
    auto *memory = static_cast<size_t *>(std::malloc(size + sizeof(size_t)));
    if (!memory) throw std::bad_alloc();
    *memory = size;
    dma_live += size;
    dma_peak = std::max(dma_peak, dma_live);
    return memory + 1;
}
void dma_free(void *memory) {
    if (!memory) return;
    auto *allocation = static_cast<size_t *>(memory) - 1;
    dma_live -= *allocation;
    std::free(allocation);
}
void mbedtls_platform_zeroize(void *buf, size_t len) { std::memset(buf, 0, len); }
int esp_aes_process_dma(esp_aes_context *, const unsigned char *input, unsigned char *output, size_t len, uint8_t *) {
    std::memcpy(output, input, len);
    return 0;
}
// One TLS record per call: encrypted through the AES-DMA helper, with
// scripted failures, closed writes, short writes and WANT results.
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *data, size_t size) {
    CHECK(ssl == &tls.ssl);
    ++tls_calls;
    tls_records.push_back(size);
    if (tls_calls == fail_tls_call) return -0x84;
    if (tls_calls == zero_tls_call) return 0;
    if (want_next) {
        want_next = false;
        return ESP_TLS_ERR_SSL_WANT_WRITE;
    }
    std::vector<unsigned char> encrypted(size);
    esp_aes_context context{};
    if (esp_aes_process_dma_ext_ram(&context, data, encrypted.data(), size, nullptr, true, true)) return -0x84;
    const auto count = short_writes ? std::min(size, size_t{7}) : size;
    wire.append(reinterpret_cast<const char *>(encrypted.data()), count);
    want_next = short_writes && count < size;
    return static_cast<int>(count);
}
void esp_http_server_dispatch_event(int32_t event_id, const void *event_data, size_t event_data_size) {
    if (event_id == HTTP_SERVER_EVENT_SENT_DATA) {
        CHECK(event_data_size == sizeof(esp_http_server_event_data));
        const auto *data = static_cast<const esp_http_server_event_data *>(event_data);
        events.push_back({event_id, data->fd, data->data_len});
    } else {
        CHECK(event_data_size == sizeof(int));
        events.push_back({event_id, *static_cast<const int *>(event_data), 0});
    }
}

// HTTPD's response head setters, over its private request state.
static httpd_req_aux &aux(httpd_req_t *r) { return *static_cast<httpd_req_aux *>(r->aux); }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
    aux(r).status = const_cast<char *>(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
    aux(r).content_type = const_cast<char *>(type);
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value) {
    auto &request = aux(r);
    CHECK(request.resp_hdrs_count < static_cast<httpd_data *>(r->handle)->config.max_resp_headers);
    request.resp_hdrs[request.resp_hdrs_count++] = {field, value};
    return ESP_OK;
}
// No scenario reads a request body.
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) { return AMITY_SEAM(httpd_req_recv, r, buf, buf_len); }
// Bounded responses never use chunk framing.
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len) {
    ++chunk_calls;
    terminators += !buf_len;
    return sdk_httpd_resp_send_chunk(r, buf, buf_len);
}
web::stall::Response::Response(httpd_req_t *, size_t size) {
    (void)armed_;
    bounded.push_back(size);
}
web::stall::Response::~Response() {}

namespace {

// HTTPS sessions send through esp-tls, which the linker routes through
// Amity's bounded write.
int send_fn(httpd_handle_t, int sockfd, const char *data, size_t size, int) {
    CHECK(sockfd == socket_fd);
    return static_cast<int>(__wrap_esp_tls_conn_write(&tls, data, size));
}

// The request HTTPD is serving, with its session.
struct Request {
    httpd_data server{};
    sock_db session{};
    httpd_req_aux::resp_hdr headers[8]{};
    Request() {
        tls.write = esp_mbedtls_write;
        server.config.max_resp_headers = 8;
        session.fd = socket_fd;
        session.handle = &server;
        session.send_fn = send_fn;
        auto &request = server.hd_req_aux;
        request.sd = &session;
        request.max_req_hdr_len = CONFIG_HTTPD_MAX_REQ_HDR_LEN;
        request.status = const_cast<char *>(HTTPD_200);
        request.content_type = const_cast<char *>(HTTPD_TYPE_TEXT);
        request.resp_hdrs = headers;
        server.hd_req.handle = &server;
        server.hd_req.aux = &request;
    }
    httpd_req_t *request() { return &server.hd_req; }
};

void reset() {
    wire.clear();
    tls_calls = fail_tls_call = zero_tls_call = chunk_calls = terminators = dma_failures = 0;
    short_writes = want_next = false;
    largest_dma_request = dma_peak = dma_live = 0;
    tls_records.clear();
    bounded.clear();
    events.clear();
}
std::string payload(size_t size) {
    std::string data(size, 'x');
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<char>('a' + i % 26);
    return data;
}
// The body the wire carries, checked against its framing.
std::string decode() {
    const auto split = wire.find("\r\n\r\n");
    CHECK(split != std::string::npos);
    if (wire.find("Transfer-Encoding: chunked\r\n") == std::string::npos) {
        const auto field = wire.find("Content-Length: ");
        CHECK(field != std::string::npos);
        CHECK(std::stoul(wire.substr(field + 16)) == wire.size() - split - 4);
        return wire.substr(split + 4);
    }
    CHECK(wire.find("Content-Length:") == std::string::npos);
    std::string decoded;
    size_t offset = split + 4;
    for (;;) {
        const auto end = wire.find("\r\n", offset);
        CHECK(end != std::string::npos);
        const auto count = std::stoul(wire.substr(offset, end - offset), nullptr, 16);
        offset = end + 2;
        CHECK(offset + count + 2 <= wire.size());
        decoded.append(wire, offset, count);
        offset += count;
        CHECK(wire.substr(offset, 2) == "\r\n");
        offset += 2;
        if (!count) {
            CHECK(offset == wire.size());
            return decoded;
        }
    }
}
bool records_bounded() {
    return std::all_of(tls_records.begin(), tls_records.end(), [](size_t count) { return count <= 512; });
}

// A 6218-byte JSON response keeps Content-Length framing: 23 TLS records of
// at most 512 bytes, the response bounded by its size, and HTTPD's events.
void bounded_records() {
    const auto large = payload(6218);
    Request normal;
    CHECK(web::json(normal.request(), large, "200 OK") == ESP_OK && decode() == large);
    CHECK(tls_calls == 23 && chunk_calls == 0 && records_bounded());
    CHECK(bounded == std::vector<size_t>({6218}));
    CHECK(events.size() == 2 && events[0].id == HTTP_SERVER_EVENT_HEADERS_SENT && events[0].fd == socket_fd);
    CHECK(events[1].id == HTTP_SERVER_EVENT_SENT_DATA && events[1].fd == socket_fd && events[1].data_len == 6218);
}

// Bodies of any size stay intact under the DMA bound, with their head.
void body_sizes() {
    for (size_t size : {size_t{6218}, size_t{0}, size_t{1}, size_t{512}, size_t{513}, size_t{40000}}) {
        reset();
        Request current;
        const auto data = payload(size);
        CHECK(web::json(current.request(), data, "202 Accepted") == ESP_OK);
        CHECK(decode() == data && dma_live == 0);
        CHECK(largest_dma_request <= 512 && dma_peak <= 1024);
        CHECK(wire.find("HTTP/1.1 202 Accepted\r\n") == 0);
        CHECK(wire.find("Content-Type: application/json\r\n") != std::string::npos);
        CHECK(wire.find("Cache-Control: no-store\r\n") != std::string::npos);
        CHECK(wire.find("X-Content-Type-Options: nosniff\r\n") != std::string::npos);
        CHECK(chunk_calls == 0 && terminators == 0 && wire.find("Transfer-Encoding:") == std::string::npos);
        CHECK(wire.find("Content-Length: " + std::to_string(size) + "\r\n") != std::string::npos);
        CHECK(records_bounded());
    }
}

// Positive short writes and WANT results lose no bytes and keep the bound.
void short_and_want_writes() {
    const auto large = payload(6218);
    Request partial;
    short_writes = true;
    CHECK(web::json(partial.request(), large, "200 OK") == ESP_OK && decode() == large && terminators == 0);
    CHECK(dma_live == 0 && largest_dma_request <= 512 && dma_peak <= 1024);
}

// HTTPD stops at the first, header, body or final TLS write error, keeping
// the emitted prefix, without retries or a success terminator.
void write_errors() {
    const auto large = payload(6218);
    Request normal;
    CHECK(web::json(normal.request(), large, "200 OK") == ESP_OK);
    const auto records = tls_calls;
    const auto expected = wire;
    for (unsigned failure : {1u, 10u, 11u, 14u, records}) {
        reset();
        Request failed;
        fail_tls_call = failure;
        CHECK(web::json(failed.request(), large, "200 OK") == ESP_ERR_HTTPD_RESP_SEND);
        CHECK(tls_calls == failure && chunk_calls == 0 && terminators == 0);
        CHECK(wire == expected.substr(0, wire.size()) && dma_live == 0);
    }
}

// A closed TLS write, which makes no progress, ends the response.
void zero_progress() {
    const auto large = payload(6218);
    Request normal;
    CHECK(web::json(normal.request(), large, "200 OK") == ESP_OK);
    const auto records = tls_calls;
    const auto expected = wire;
    for (unsigned stopped : {1u, 11u, records}) {
        reset();
        Request closed;
        zero_tls_call = stopped;
        CHECK(web::json(closed.request(), large, "200 OK") == ESP_ERR_HTTPD_RESP_SEND);
        CHECK(tls_calls == stopped && wire == expected.substr(0, wire.size()) && dma_live == 0);
        CHECK(chunk_calls == 0 && terminators == 0);
    }
}

// A hard DMA allocation failure surfaces instead of being retried or hidden.
void dma_failure() {
    const auto large = payload(6218);
    Request hard;
    dma_limit = 128;
    CHECK(web::json(hard.request(), large, "200 OK") != ESP_OK);
    CHECK(dma_failures == 1 && chunk_calls == 0 && terminators == 0 && dma_live == 0);
}

// The bounded write passes WANT results on, rejects bad arguments and
// writes nothing for an empty write.
void bounded_write() {
    const auto large = payload(6218);
    Request wants;
    want_next = true;
    CHECK(__wrap_esp_tls_conn_write(&tls, large.data(), large.size()) == ESP_TLS_ERR_SSL_WANT_WRITE);
    CHECK(tls_calls == 1 && wire.empty());
    reset();
    CHECK(__wrap_esp_tls_conn_write(nullptr, large.data(), large.size()) == -1);
    CHECK(__wrap_esp_tls_conn_write(&tls, nullptr, 1) == -1 && tls_calls == 0);
    CHECK(__wrap_esp_tls_conn_write(&tls, large.data(), 0) == 0 && tls_calls == 0);
    // A long write is cut to one 512-byte record.
    CHECK(__wrap_esp_tls_conn_write(&tls, large.data(), large.size()) == 512);
    CHECK(tls_calls == 1 && wire == large.substr(0, 512));
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"bounded-records", bounded_records},
                            {"body-sizes", body_sizes},
                            {"short-and-want-writes", short_and_want_writes},
                            {"write-errors", write_errors},
                            {"zero-progress", zero_progress},
                            {"dma-failure", dma_failure},
                            {"bounded-write", bounded_write}});
}
