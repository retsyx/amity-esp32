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
// HTTPD serves every session from one task, and its receive timeout restarts
// with each TLS fragment. A total deadline for each TLS handshake, request
// head and leftover-body purge bounds how long one slow client can hold all
// others. Expiry shuts the socket down, ending its blocked receive.
namespace web::stall {
constexpr uint64_t handshake_us=5000000,head_us=5000000,purge_us=5000000;
// Creates the deadline timer before the HTTPS server can accept a connection.
esp_err_t prepare();
// Bounds the request heads and purges of this server only.
void serve(httpd_handle_t server);
void release(httpd_handle_t server);
// Used only by the HTTPS server task, one phase at a time.
void arm(int socket,uint64_t timeout_us);
void disarm();
}
