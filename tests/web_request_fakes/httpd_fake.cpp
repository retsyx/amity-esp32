/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HTTP server fake. Every httpd_req_t a firmware handler receives is an
// amity_fake::HttpRequest.
#include "httpd_fake.hpp"
#include "seams.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <strings.h>

namespace {

using amity_fake::HttpRequest;
using amity_fake::HttpResponse;

[[noreturn]] void fail(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

std::mutex lock;
std::vector<amity_fake::UriHandler> handlers;
std::set<HttpRequest *> asynchronous;

HttpRequest &request(httpd_req_t *r, const char *function) {
    if (!r) fail(std::string(function) + " with a null request");
    return *static_cast<HttpRequest *>(r);
}

bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

const std::string *find_header(const std::map<std::string, std::string> &headers, const char *field) {
    for (const auto &header : headers)
        if (same(header.first, field)) return &header.second;
    return nullptr;
}

// Sends the head: the response takes the status, type and headers now.
void send_head(HttpRequest &value) {
    HttpResponse &response = *value.response;
    if (response.head_sent) return;
    response.head_sent = true;
    if (value.pending_status) response.status = value.pending_status;
    if (value.pending_type) response.type = value.pending_type;
    for (const auto &header : value.pending_headers) response.headers.emplace_back(header.first, header.second);
}

}  // namespace

namespace amity_fake {

HttpRequest::HttpRequest(int method_value, std::string_view uri_value, std::string body_value)
    : httpd_req_t{}, body(std::move(body_value)) {
    method = method_value;
    // HTTPD writes the URI into its request once, before any handler runs.
    const std::size_t size = std::min(uri_value.size(), std::size_t(HTTPD_MAX_URI_LEN));
    std::memcpy(const_cast<char *>(uri), uri_value.data(), size);
    content_len = body.size();
}

std::string HttpResponse::header(std::string_view name) const {
    for (const auto &header : headers)
        if (same(header.first, name)) return header.second;
    return {};
}
bool HttpResponse::has_header(std::string_view name) const {
    return std::any_of(headers.begin(), headers.end(), [&](const auto &header) { return same(header.first, name); });
}

std::vector<UriHandler> uri_handlers() {
    std::lock_guard<std::mutex> guard(lock);
    return handlers;
}
std::size_t pending_async_requests() {
    std::lock_guard<std::mutex> guard(lock);
    return asynchronous.size();
}

}  // namespace amity_fake

extern "C" {

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler) {
    if (!handle || !uri_handler || !uri_handler->uri || !uri_handler->handler) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> guard(lock);
    for (const auto &existing : handlers)
        if (existing.server == handle && existing.uri == uri_handler->uri && existing.method == uri_handler->method)
            return ESP_ERR_HTTPD_HANDLER_EXISTS;
    handlers.push_back({handle, uri_handler->uri, uri_handler->method, uri_handler->handler, uri_handler->user_ctx});
    return ESP_OK;
}

bool httpd_uri_match_wildcard(const char *uri_template, const char *uri_to_match, size_t match_upto) {
    return AMITY_SEAM(httpd_uri_match_wildcard, uri_template, uri_to_match, match_upto);
}

esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out) {
    if (!r || !out) return ESP_ERR_INVALID_ARG;
    auto *copy = new HttpRequest(request(r, "httpd_req_async_handler_begin"));
    copy->asynchronous = true;
    std::lock_guard<std::mutex> guard(lock);
    asynchronous.insert(copy);
    *out = copy;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_complete(httpd_req_t *r) {
    if (!r) return ESP_ERR_INVALID_ARG;
    auto *copy = &request(r, "httpd_req_async_handler_complete");
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!asynchronous.erase(copy)) fail("httpd_req_async_handler_complete of a request that is not an asynchronous copy");
    }
    delete copy;
    return ESP_OK;
}

int httpd_req_to_sockfd(httpd_req_t *r) { return r ? request(r, "httpd_req_to_sockfd").socket : -1; }

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) {
    HttpRequest &value = request(r, "httpd_req_recv");
    ++value.receive_calls;
    if (value.receive) return value.receive(buf, buf_len);
    const std::size_t remaining = value.offset < value.content_len ? value.content_len - value.offset : 0;
    const std::size_t available = value.offset < value.body.size() ? value.body.size() - value.offset : 0;
    const std::size_t count = std::min({buf_len, value.chunk, remaining, available});
    std::memcpy(buf, value.body.data() + value.offset, count);
    value.offset += count;
    return int(count);
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field) {
    const std::string *found = find_header(request(r, "httpd_req_get_hdr_value_len").headers, field);
    return found ? found->size() : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size) {
    if (!r || !field) return ESP_ERR_INVALID_ARG;
    const std::string *found = find_header(request(r, "httpd_req_get_hdr_value_str").headers, field);
    if (!found) return ESP_ERR_NOT_FOUND;
    // strlcpy: as much as fits, always terminated.
    if (val_size) {
        const std::size_t copied = std::min(found->size(), val_size - 1);
        std::memcpy(val, found->data(), copied);
        val[copied] = '\0';
    }
    return val_size < found->size() + 1 ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
    if (!r || !status) return ESP_ERR_INVALID_ARG;
    request(r, "httpd_resp_set_status").pending_status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
    if (!r || !type) return ESP_ERR_INVALID_ARG;
    request(r, "httpd_resp_set_type").pending_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value) {
    if (!r || !field || !value) return ESP_ERR_INVALID_ARG;
    HttpRequest &target = request(r, "httpd_resp_set_hdr");
    if (target.pending_headers.size() >= target.max_response_headers) return ESP_ERR_HTTPD_RESP_HDR;
    target.pending_headers.emplace_back(field, value);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len) {
    if (!r) return ESP_ERR_INVALID_ARG;
    HttpRequest &value = request(r, "httpd_resp_send");
    HttpResponse &response = *value.response;
    if (response.head_sent) fail("httpd_resp_send after the response head was sent");
    if (value.send_error != ESP_OK) return value.send_error;
    send_head(value);
    const std::size_t size = buf_len == HTTPD_RESP_USE_STRLEN ? (buf ? std::strlen(buf) : 0) : std::size_t(buf_len);
    if (size) response.body.assign(buf, size);
    ++response.sends;
    response.finished = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len) {
    if (!r) return ESP_ERR_INVALID_ARG;
    HttpRequest &value = request(r, "httpd_resp_send_chunk");
    HttpResponse &response = *value.response;
    if (response.finished) fail("httpd_resp_send_chunk after the response was complete");
    if (response.head_sent && !response.chunked) fail("httpd_resp_send_chunk after httpd_resp_send");
    if (value.send_error != ESP_OK) return value.send_error;
    send_head(value);
    response.chunked = true;
    const std::size_t size = buf_len == HTTPD_RESP_USE_STRLEN ? (buf ? std::strlen(buf) : 0) : std::size_t(buf_len);
    ++response.sends;
    if (!buf || !size) {
        response.finished = true;
        return ESP_OK;
    }
    response.body.append(buf, size);
    return ESP_OK;
}

}  // extern "C"
