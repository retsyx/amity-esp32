# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

set(QR_CODE_DIR "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/examples/common/QRCode/repo/c")
idf_component_register(SRCS "web_admin.cpp" "configuration.cpp" "${QR_CODE_DIR}/qrcodegen.c" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${QR_CODE_DIR}"
    REQUIRES web device_settings config_store amity_hub ble_remote amity_matter amity_homekit system_services espressif__cjson esp_wifi esp_app_format
    EMBED_FILES "assets/index.html" "assets/app.js" "assets/style.css")
