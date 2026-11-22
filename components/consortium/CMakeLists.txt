idf_component_register(SRCS "json.cpp" "wire.cpp" "owned_intent.cpp" "session.cpp" "radio.cpp" "bootstrap.cpp" "settings.cpp" "update.cpp" "enrollment.cpp"
    INCLUDE_DIRS "include" REQUIRES amity_system config_store device_settings mbedtls nvs_flash esp_timer esp_app_format lwip esp_netif mdns bt chip network_diagnostics vfs)
# Operational platform adapters use the existing CHIP-owned radio/network stack.
if(ESP_PLATFORM)
    target_sources(${COMPONENT_LIB} PRIVATE "credentials.cpp" "tls_link.cpp" "runtime.cpp" "admission_crypto.cpp" "gatt_transport.cpp")
endif()
