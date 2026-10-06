idf_component_register(SRCS "homekit.cpp" "setup.c" INCLUDE_DIRS "include"
    REQUIRES amity_hub amity_matter config_store device_settings ble_remote
    PRIV_REQUIRES esp_hap_core esp_hap_apple_profiles mu_srp mbedtls esp_wifi esp_app_format)
