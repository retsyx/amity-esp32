idf_component_register(SRCS "json.cpp" "wire.cpp" "session.cpp" "lifecycle.cpp" "radio.cpp" "bindings.cpp" "bootstrap.cpp" "bootstrap_record.cpp" "transfer.cpp" "control.cpp" "settings.cpp" "update.cpp" "relay.cpp" "enrollment.cpp"
    INCLUDE_DIRS "include" REQUIRES amity_system config_store ble_remote mbedtls nvs_flash esp_timer esp_app_format lwip esp_netif mdns protocomm bt chip protobuf-c)
# Operational platform adapters use the existing CHIP-owned radio/network stack.
if(ESP_PLATFORM)
    target_sources(${COMPONENT_LIB} PRIVATE "credentials.cpp" "tls_link.cpp" "runtime.cpp" "security2_client.cpp" "gatt_transport.cpp")
endif()
