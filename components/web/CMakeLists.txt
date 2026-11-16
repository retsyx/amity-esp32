# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# The HTTPS deadline guard reads HTTPD's private session state.
set(httpd_dir "$ENV{IDF_PATH}/components/esp_http_server")
idf_component_register(SRCS "web.cpp" "redirect.cpp" "stall.cpp" "credentials.cpp" "request.cpp" "tls_write.cpp" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${httpd_dir}/src" "${httpd_dir}/src/port/esp32" "${httpd_dir}/src/util"
    REQUIRES esp_https_server mbedtls config_store esp_timer esp_system lwip)

# Apply the TCP policy before HTTPS performs its first TLS handshake write.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_tls_server_session_create")
# The wrapper is a separate archive member without an ordinary application
# reference. Retain it explicitly before resolving the SDK's wrapped calls.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_tls_conn_write"
    "-Wl,--undefined=__wrap_esp_tls_conn_write")
# Bound the HTTPS server's request heads and leftover-body purges.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=httpd_req_new" "-Wl,--undefined=__wrap_httpd_req_new"
    "-Wl,--wrap=httpd_uri" "-Wl,--undefined=__wrap_httpd_uri"
    "-Wl,--wrap=httpd_req_delete" "-Wl,--undefined=__wrap_httpd_req_delete")
