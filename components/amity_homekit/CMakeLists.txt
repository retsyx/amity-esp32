# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "homekit.cpp" "setup.c" INCLUDE_DIRS "include"
    REQUIRES amity_hub amity_matter config_store device_settings ble_remote
    PRIV_REQUIRES esp_hap_core esp_hap_apple_profiles mu_srp mbedtls esp_wifi esp_app_format)
