idf_component_register(SRCS "device_settings.cpp" "factory_reset.cpp" "reset_button.cpp" INCLUDE_DIRS "include"
    REQUIRES config_store PRIV_REQUIRES nvs_flash esp_partition mbedtls esp_driver_gpio esp_timer esp_system)
