idf_component_register(SRCS "engine.cpp" "bus.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_gptimer esp_timer)
