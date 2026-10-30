# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "config_store.cpp" "production_storage.cpp" "system_identity.cpp" "system_configuration.cpp"
    "configuration_journal.cpp" "configuration_replacement.cpp" "candidate_network.cpp" "system_configuration_nvs.cpp" INCLUDE_DIRS "include"
    REQUIRES nvs_flash amity_system PRIV_REQUIRES esp_hw_support efuse bootloader_support espressif__cjson mbedtls)
