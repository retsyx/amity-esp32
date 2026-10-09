# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "device_settings.cpp" "factory_reset.cpp" "reset_button.cpp" INCLUDE_DIRS "include"
    REQUIRES config_store PRIV_REQUIRES nvs_flash esp_partition mbedtls esp_driver_gpio esp_timer esp_system)
