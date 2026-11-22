# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

set(QR_CODE_DIR "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/examples/common/QRCode/repo/c")
idf_component_register(SRCS "web_admin.cpp" "system_admin.cpp" "activity_editor.cpp" "activity_editor_model.cpp" "${QR_CODE_DIR}/qrcodegen.c" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${QR_CODE_DIR}"
    REQUIRES web device_settings config_store amity_hub ble_remote amity_matter amity_homekit system_services consortium radio_control system_sync espressif__cjson esp_wifi esp_app_format)
# The UI is served gzip-encoded; only the compressed assets are embedded.
idf_build_get_property(python PYTHON)
foreach(asset index.html app.js system.js style.css favicon.svg)
    set(compressed "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(OUTPUT "${compressed}"
        COMMAND "${python}" "${CMAKE_SOURCE_DIR}/tools/compress_asset.py" "${CMAKE_CURRENT_LIST_DIR}/assets/${asset}" "${compressed}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/assets/${asset}" "${CMAKE_SOURCE_DIR}/tools/compress_asset.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${compressed}" BINARY DEPENDS "${compressed}")
endforeach()
