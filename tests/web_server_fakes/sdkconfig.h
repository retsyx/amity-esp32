/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The Amity build's configuration values that the web server code reads
// (sdk_parity/web_server.def).
#define CONFIG_LWIP_IPV6 1
#define CONFIG_HTTPD_MAX_REQ_HDR_LEN 1024
#define CONFIG_HTTPD_MAX_URI_LEN 256
#define CONFIG_HTTPD_ENABLE_EVENTS 1
