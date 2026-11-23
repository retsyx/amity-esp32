/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the HTTP server fake: requests a scenario hands to a
// handler, and what the handler sent back.
#include "esp_http_server.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amity_fake {

// The response to one request. The status, type and headers are those set
// when the head went out with the first send.
struct HttpResponse {
    bool head_sent = false, chunked = false;
    // Sent completely: one httpd_resp_send, or the final empty chunk.
    bool finished = false;
    std::string status = "200 OK", type = "text/html";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    unsigned sends = 0;
    // The first value of a header, matched without case; empty when absent.
    std::string header(std::string_view name) const;
    bool has_header(std::string_view name) const;
};

// A request as HTTPD hands it to a handler. The members after the SDK's
// fields script the request; response records what was sent, including
// through an asynchronous copy of the request.
struct HttpRequest : httpd_req_t {
    explicit HttpRequest(int method = HTTP_GET, std::string_view uri = "/", std::string body = {});
    // Request headers, matched without case.
    std::map<std::string, std::string> headers;
    // Read by httpd_req_recv, never past content_len, which starts as its size.
    std::string body;
    std::size_t offset = 0;
    // The most bytes one httpd_req_recv returns.
    std::size_t chunk = SIZE_MAX;
    // Replaces reading body when set.
    std::function<int(char *, std::size_t)> receive;
    unsigned receive_calls = 0;
    // What httpd_req_to_sockfd returns.
    int socket = -1;
    // Returned by every send, which then sends nothing.
    esp_err_t send_error = ESP_OK;
    // The server's limit on headers a handler adds; HTTPD's default.
    std::size_t max_response_headers = 8;
    std::shared_ptr<HttpResponse> response = std::make_shared<HttpResponse>();
    // A copy made by httpd_req_async_handler_begin.
    bool asynchronous = false;
    // The caller's pointers, kept until the head is sent, as HTTPD keeps them.
    const char *pending_status = nullptr, *pending_type = nullptr;
    std::vector<std::pair<const char *, const char *>> pending_headers;
};

struct UriHandler {
    httpd_handle_t server;
    std::string uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
};
// Registered handlers, in registration order.
std::vector<UriHandler> uri_handlers();
// Asynchronous copies begun and not yet completed.
std::size_t pending_async_requests();

}  // namespace amity_fake
