idf_component_register(SRCS "ble_remote.cpp" "hid_keyboard.cpp" "atomic_remote.c" "atomic_controller.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_timer nvs_flash)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
