idf_component_register(SRCS "json.cpp" "wire.cpp" "owned_intent.cpp" "session.cpp" "lifecycle.cpp" "radio.cpp" "bootstrap.cpp" "transfer.cpp" "settings.cpp" "update.cpp" "enrollment.cpp"
    INCLUDE_DIRS "include" REQUIRES amity_system config_store device_settings ble_remote mbedtls nvs_flash esp_timer esp_app_format lwip esp_netif mdns protocomm bt chip protobuf-c network_diagnostics)
# Operational platform adapters use the existing CHIP-owned radio/network stack.
if(ESP_PLATFORM)
    target_sources(${COMPONENT_LIB} PRIVATE "credentials.cpp" "tls_link.cpp" "runtime.cpp" "security2_client.cpp" "admission_crypto.cpp" "gatt_transport.cpp")
endif()
