/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// HTTPD request accessors as the pinned ESP-IDF defines them over its private
// layout, for binaries that build HTTPD's state instead of linking the HTTP
// server fake.
#include "esp_httpd_priv.h"

// CONFIG_HTTPD_VALIDATE_REQ is off in the Amity build, so a request is not
// checked against the server task.
int httpd_req_to_sockfd(httpd_req_t *r) {
    if (!r) return -1;
    return static_cast<httpd_req_aux *>(r->aux)->sd->fd;
}
