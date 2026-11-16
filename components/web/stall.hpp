/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_http_server.h"
#include <cstdint>
// HTTPD serves every session from one task, and its socket timeouts restart
// with each TLS fragment. A total deadline for each TLS handshake, request
// head, response and leftover-body purge bounds how long one slow client can
// hold all others. Expiry shuts the socket down, ending its blocked transfer.
namespace web::stall {
constexpr uint64_t handshake_us=5000000,head_us=5000000;
// A response or unread body of known size gets a base allowance plus time at
// a minimum rate, so a client transferring at least that fast is never cut
// off, however large the transfer. Request bodies read by handlers, such as
// firmware uploads, keep their own deadlines.
constexpr uint64_t transfer_base_us=5000000,minimum_bytes_per_second=16384;
constexpr uint64_t transfer_us(uint64_t bytes) {return transfer_base_us+bytes*1000000/minimum_bytes_per_second;}
// Creates the deadline timer before the HTTPS server can accept a connection.
esp_err_t prepare();
// Bounds the request heads and purges of this server only.
void serve(httpd_handle_t server);
void release(httpd_handle_t server);
// Used only by the HTTPS server task, one phase at a time.
void arm(int socket,uint64_t timeout_us);
void disarm();
// Bounds one response sent by an HTTPS handler on the server task. Elsewhere,
// including responses streamed from other tasks, it does nothing.
class Response {
public:
    Response(httpd_req_t *request,size_t size);
    ~Response();
    Response(const Response &)=delete;
    Response &operator=(const Response &)=delete;
private:
    bool armed_=false;
};
}
