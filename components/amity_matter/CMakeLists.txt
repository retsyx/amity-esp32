# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "amity_matter.cpp" "commissioning_credentials.cpp" "diagnostics.cpp" "apple_fabrics.cpp" "consortium_network.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_matter
                       PRIV_REQUIRES nvs_flash device_settings esp_netif esp_event esp_wifi system_services consortium config_store)

# Preserve one shared CHIP host while permanently suppressing member home-service publication.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=_ZN4chip3app11DnssdServer11StartServerEv")
