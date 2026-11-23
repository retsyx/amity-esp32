/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HTTP server fake scripts requests and records responses as HTTPD would.
#include "httpd_fake.hpp"
#include "seams.hpp"
#include <cstring>
#include <string>

namespace {

using amity_fake::HttpRequest;

void request_reads() {
    HttpRequest request(HTTP_POST, "/api/config", "0123456789");
    CHECK(request.method == HTTP_POST && std::strcmp(request.uri, "/api/config") == 0 && request.content_len == 10);
    request.chunk = 4;
    request.content_len = 9;
    char buffer[16];
    CHECK(httpd_req_recv(&request, buffer, sizeof(buffer)) == 4 && std::memcmp(buffer, "0123", 4) == 0);
    CHECK(httpd_req_recv(&request, buffer, 2) == 2 && httpd_req_recv(&request, buffer, sizeof(buffer)) == 3);
    CHECK(std::memcmp(buffer, "678", 3) == 0 && httpd_req_recv(&request, buffer, sizeof(buffer)) == 0);
    CHECK(request.receive_calls == 4 && request.offset == 9);
    request.receive = [](char *, size_t) { return HTTPD_SOCK_ERR_TIMEOUT; };
    CHECK(httpd_req_recv(&request, buffer, sizeof(buffer)) == HTTPD_SOCK_ERR_TIMEOUT && request.receive_calls == 5);
    CHECK(httpd_req_to_sockfd(&request) == -1 && httpd_req_to_sockfd(nullptr) == -1);
    request.socket = 9;
    CHECK(httpd_req_to_sockfd(&request) == 9);
    const std::string long_uri(400, 'a');
    HttpRequest truncated(HTTP_GET, long_uri);
    CHECK(std::strlen(truncated.uri) == HTTPD_MAX_URI_LEN);
}

void header_lookup() {
    HttpRequest request;
    request.headers["Authorization"] = "Bearer token";
    CHECK(httpd_req_get_hdr_value_len(&request, "authorization") == 12 && httpd_req_get_hdr_value_len(&request, "Host") == 0);
    char value[13];
    CHECK(httpd_req_get_hdr_value_str(&request, "AUTHORIZATION", value, sizeof(value)) == ESP_OK);
    CHECK(std::strcmp(value, "Bearer token") == 0);
    char small[7];
    CHECK(httpd_req_get_hdr_value_str(&request, "Authorization", small, sizeof(small)) == ESP_ERR_HTTPD_RESULT_TRUNC);
    CHECK(std::strcmp(small, "Bearer") == 0);
    CHECK(httpd_req_get_hdr_value_str(&request, "Cookie", value, sizeof(value)) == ESP_ERR_NOT_FOUND);
}

void response_head() {
    HttpRequest request;
    {
        // The handler's strings must outlive the send, as with HTTPD.
        std::string status = "201 Created";
        CHECK(httpd_resp_set_status(&request, status.c_str()) == ESP_OK);
        CHECK(httpd_resp_set_type(&request, HTTPD_TYPE_JSON) == ESP_OK);
        CHECK(httpd_resp_set_hdr(&request, "Cache-Control", "no-store") == ESP_OK);
        CHECK(!request.response->head_sent && request.response->status == "200 OK");
        CHECK(httpd_resp_sendstr(&request, "{}") == ESP_OK);
    }
    const auto &response = *request.response;
    CHECK(response.head_sent && response.finished && response.sends == 1 && response.status == "201 Created");
    CHECK(response.type == "application/json" && response.header("cache-control") == "no-store");
    CHECK(response.body == "{}" && !response.has_header("Connection") && !response.chunked);
    HttpRequest limited;
    for (int i = 0; i < 8; ++i) CHECK(httpd_resp_set_hdr(&limited, "X-Header", "value") == ESP_OK);
    CHECK(httpd_resp_set_hdr(&limited, "X-Header", "value") == ESP_ERR_HTTPD_RESP_HDR);
    HttpRequest failing;
    failing.send_error = ESP_ERR_HTTPD_RESP_SEND;
    CHECK(httpd_resp_send(&failing, "lost", 4) == ESP_ERR_HTTPD_RESP_SEND && !failing.response->head_sent);
}

void chunked() {
    HttpRequest request;
    CHECK(httpd_resp_set_type(&request, HTTPD_TYPE_OCTET) == ESP_OK);
    CHECK(httpd_resp_send_chunk(&request, "abc", 3) == ESP_OK && httpd_resp_sendstr_chunk(&request, "de") == ESP_OK);
    CHECK(request.response->chunked && !request.response->finished && request.response->body == "abcde");
    CHECK(httpd_resp_sendstr_chunk(&request, nullptr) == ESP_OK && request.response->finished);
    CHECK(request.response->sends == 3 && request.response->type == "application/octet-stream");
}

void send_after_send() {
    HttpRequest request;
    CHECK(httpd_resp_sendstr(&request, "first") == ESP_OK);
    httpd_resp_sendstr(&request, "second");
}

void asynchronous() {
    HttpRequest request(HTTP_GET, "/api/logs");
    request.socket = 5;
    CHECK(httpd_resp_set_hdr(&request, "Content-Disposition", "attachment") == ESP_OK);
    httpd_req_t *copy = nullptr;
    CHECK(httpd_req_async_handler_begin(&request, &copy) == ESP_OK && copy && copy != &request);
    CHECK(amity_fake::pending_async_requests() == 1 && static_cast<HttpRequest *>(copy)->asynchronous);
    CHECK(std::strcmp(copy->uri, "/api/logs") == 0 && httpd_req_to_sockfd(copy) == 5);
    CHECK(httpd_resp_send_chunk(copy, "log", 3) == ESP_OK && httpd_resp_send_chunk(copy, nullptr, 0) == ESP_OK);
    CHECK(httpd_req_async_handler_complete(copy) == ESP_OK && amity_fake::pending_async_requests() == 0);
    CHECK(request.response->body == "log" && request.response->finished);
    CHECK(request.response->header("Content-Disposition") == "attachment");
}

void complete_original() {
    HttpRequest request;
    httpd_req_async_handler_complete(&request);
}

esp_err_t handle(httpd_req_t *) { return ESP_OK; }

void handlers() {
    int server = 0, other = 0;
    httpd_uri_t uri{"/api/*", HTTP_GET, handle, &server};
    CHECK(httpd_register_uri_handler(&server, &uri) == ESP_OK);
    CHECK(httpd_register_uri_handler(&server, &uri) == ESP_ERR_HTTPD_HANDLER_EXISTS);
    CHECK(httpd_register_uri_handler(&other, &uri) == ESP_OK);
    uri.method = HTTP_POST;
    CHECK(httpd_register_uri_handler(&server, &uri) == ESP_OK);
    const auto registered = amity_fake::uri_handlers();
    CHECK(registered.size() == 3 && registered[0].server == &server && registered[0].uri == "/api/*");
    CHECK(registered[2].method == HTTP_POST && registered[2].handler == handle && registered[2].user_ctx == &server);
}

void wildcard_seam() {
    amity_seam::install<&httpd_uri_match_wildcard>(
        [](const char *pattern, const char *uri, size_t) { return std::strcmp(pattern, uri) == 0; });
    CHECK(httpd_uri_match_wildcard("/a", "/a", 2) && !httpd_uri_match_wildcard("/a", "/b", 2));
    CHECK(amity_seam::count("httpd_uri_match_wildcard") == 2);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"request-reads", request_reads},
                            {"header-lookup", header_lookup},
                            {"response-head", response_head},
                            {"chunked", chunked},
                            {"send-after-send", send_after_send},
                            {"asynchronous", asynchronous},
                            {"complete-original", complete_original},
                            {"handlers", handlers},
                            {"wildcard-seam", wildcard_seam}});
}
