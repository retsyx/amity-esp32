/*
 * Copyright 2026.
 * This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define HAP_HTTP_BODY_TIMEOUT_US (10ULL * 1000ULL * 1000ULL)

/* Read a caller-bounded body before taking the global HAP lock. The buffer size
 * must equal Content-Length. Failure shuts down the connection so unread bytes
 * cannot become another request. The deadline also bounds encrypted-frame reads.
 */
esp_err_t hap_http_read_body(httpd_req_t *req, char *buffer, size_t size);
