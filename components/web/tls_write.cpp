// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "esp_tls.h"
#include <algorithm>

extern "C" ssize_t __real_esp_tls_conn_write(esp_tls_t*, const void*, size_t);
extern "C" ssize_t __wrap_esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t size) {
    // AES copies PSRAM records through scarce internal DMA buffers. Bound
    // application writes at the public TLS seam, whose contract permits a
    // positive short write. HTTPD sends the remainder without adding HTTP
    // chunks and their separate tiny TLS records. Handshake buffers stay full.
    const auto written=__real_esp_tls_conn_write(tls,data,std::min(size,size_t{512}));
    // HTTPD's send-all loop advances only on positive results. A closed TLS
    // write must terminate that response instead of spinning without progress.
    return size&&written==0?-1:written;
}
