# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

cmake_minimum_required(VERSION 3.22)
set(PROJECT_VER "0.1.49" CACHE STRING "Firmware version")
set(PROJECT_VER_NUMBER 50 CACHE STRING "Matter firmware version number")
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Source tools/env.sh before building")
endif()
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
set(EXTRA_COMPONENT_DIRS "${ESP_MATTER_PATH}/components" "${MATTER_SDK_PATH}/config/esp32/components")
foreach(HAP_COMPONENT esp_hap_core esp_hap_platform esp_hap_apple_profiles hkdf-sha mu_srp)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/third_party/esp-homekit-sdk/components/${HAP_COMPONENT}")
endforeach()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(amity)
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)

# Release package consumed by the standalone Go provisioning tool.
add_custom_target(provisioning-package
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/package_firmware.py --build ${CMAKE_BINARY_DIR}
    DEPENDS gen_project_binary bootloader partition_table_bin ${CMAKE_BINARY_DIR}/ota_data_initial.bin
    VERBATIM)
