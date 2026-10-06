idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES device_settings amity_hub ble_remote amity_matter amity_homekit config_store web_admin system_services espressif__mdns esp_event)
