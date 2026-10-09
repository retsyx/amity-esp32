# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

idf_component_register(SRCS "ble_remote.cpp" "hid_keyboard.cpp" "atomic_remote.c" "atomic_controller.cpp" "siri_remote.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_timer nvs_flash)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
