idf_component_register(SRCS "synchronization_codec.cpp" "system_sync.cpp" INCLUDE_DIRS "include"
    REQUIRES amity_hub config_store consortium esp_timer freertos)
