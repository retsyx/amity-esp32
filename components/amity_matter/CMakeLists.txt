idf_component_register(SRCS "amity_matter.cpp" "commissioning_credentials.cpp" "diagnostics.cpp" "apple_fabrics.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_matter
                       PRIV_REQUIRES nvs_flash device_settings esp_netif esp_event esp_wifi system_services)
