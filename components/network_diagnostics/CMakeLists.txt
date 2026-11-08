# Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "network_diagnostics.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer log)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_wifi_internal_tx"
    "-Wl,--wrap=esp_netif_receive")
