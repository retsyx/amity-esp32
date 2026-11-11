# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "web.cpp" "credentials.cpp" "request.cpp" "tls_write.cpp" INCLUDE_DIRS "include" REQUIRES esp_https_server mbedtls config_store esp_timer esp_system lwip)

# Apply the TCP policy before HTTPS performs its first TLS handshake write.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_tls_server_session_create")
# The wrapper is a separate archive member without an ordinary application
# reference. Retain it explicitly before resolving the SDK's wrapped calls.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_tls_conn_write"
    "-Wl,--undefined=__wrap_esp_tls_conn_write")
