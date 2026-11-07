# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

cmake_minimum_required(VERSION 3.22)
set(PROJECT_VER "0.1.148")
set(PROJECT_VER_NUMBER 149)
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

add_custom_target(check-firmware-layouts ALL
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/check_firmware_layouts.py
        --elf ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf
        --output ${CMAKE_BINARY_DIR}/firmware-layouts.json
    DEPENDS ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf VERBATIM)

set(AMITY_RELEASE_SIGNING_KEY "" CACHE FILEPATH "Separate P-256 release-manifest signing key")
set(AMITY_RELEASE_ARGUMENTS --profile development)
set(AMITY_BUNDLE_ARGUMENTS)
if(CONFIG_AMITY_PRODUCTION_UPDATES)
    if(NOT EXISTS "${AMITY_RELEASE_SIGNING_KEY}")
        message(FATAL_ERROR "Production releases require AMITY_RELEASE_SIGNING_KEY; native boot signing uses its separate SDK key")
    endif()
    set(AMITY_RELEASE_ARGUMENTS --profile production --signing-key "${AMITY_RELEASE_SIGNING_KEY}")
    set(AMITY_BUNDLE_ARGUMENTS --signature "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.release.sig")
endif()
add_custom_target(ota-release-manifest ALL
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/release_manifest.py
        --image ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin ${AMITY_RELEASE_ARGUMENTS}
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/update_bundle.py
        --image ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin
        --manifest ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.release.json
        --output ${CMAKE_BINARY_DIR}/amity-update.bin ${AMITY_BUNDLE_ARGUMENTS}
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/partition_image.py
        --image ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin
        --manifest ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.release.json
        --output ${CMAKE_BINARY_DIR}/${PROJECT_NAME}-partition.bin ${AMITY_BUNDLE_ARGUMENTS}
    BYPRODUCTS ${CMAKE_BINARY_DIR}/${PROJECT_NAME}-partition.bin
    DEPENDS gen_project_binary check-firmware-layouts VERBATIM)

# Direct USB flashing and provisioning must install the same self-contained OTA
# partition as remote staging, so a first-boot primary can update its members.
foreach(AMITY_FLASH_TARGET flash app-flash encrypted-flash encrypted-app-flash)
    if(TARGET ${AMITY_FLASH_TARGET})
        foreach(AMITY_FLASH_PROPERTY FLASH_FILE FLASH_ENTRY IMAGES ENCRYPTED_IMAGES NON_ENCRYPTED_IMAGES)
            get_property(AMITY_FLASH_VALUE TARGET ${AMITY_FLASH_TARGET} PROPERTY ${AMITY_FLASH_PROPERTY})
            if(AMITY_FLASH_VALUE)
                string(REPLACE "$<TARGET_PROPERTY:__idf_build_target,EXECUTABLE_NAME>.bin" "${PROJECT_NAME}-partition.bin" AMITY_FLASH_VALUE "${AMITY_FLASH_VALUE}")
                string(REPLACE "${PROJECT_NAME}.bin" "${PROJECT_NAME}-partition.bin" AMITY_FLASH_VALUE "${AMITY_FLASH_VALUE}")
                set_property(TARGET ${AMITY_FLASH_TARGET} PROPERTY ${AMITY_FLASH_PROPERTY} "${AMITY_FLASH_VALUE}")
            endif()
        endforeach()
        add_dependencies(${AMITY_FLASH_TARGET} ota-release-manifest)
    endif()
endforeach()

# Release package consumed by the standalone Go provisioning tool.
add_custom_target(provisioning-package
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/package_firmware.py --build ${CMAKE_BINARY_DIR}
    DEPENDS ota-release-manifest bootloader partition_table_bin ${CMAKE_BINARY_DIR}/ota_data_initial.bin
    VERBATIM)
