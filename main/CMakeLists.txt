# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES device_settings amity_hub ble_remote amity_matter amity_homekit config_store web_admin system_services espressif__mdns esp_event)
