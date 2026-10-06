set(QR_CODE_DIR "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/examples/common/QRCode/repo/c")
idf_component_register(SRCS "web_admin.cpp" "configuration.cpp" "${QR_CODE_DIR}/qrcodegen.c" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${QR_CODE_DIR}"
    REQUIRES web device_settings config_store amity_hub ble_remote amity_matter amity_homekit system_services espressif__cjson esp_wifi esp_app_format
    EMBED_FILES "assets/index.html" "assets/app.js" "assets/style.css")
