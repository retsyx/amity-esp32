# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# HomeKit and the HomeKit SDK fork.
if(AMITY_TEST_TIER STREQUAL standalone)
    # HomeKit SDK fork
    amity_test(homekit_input_test
        SOURCES "${T}/homekit_input_test.cpp"
            "${HAP}/esp_hap_core/src/esp_hap_pair_common.c" "${HAP}/esp_hap_core/src/esp_hap_pairings.c"
        INCLUDES "${T}/homekit_input_fakes" "${HAP}/esp_hap_core/src/priv_includes")
    set_target_properties(homekit_input_test PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    set_source_files_properties(
        "${HAP}/esp_hap_core/src/esp_hap_pair_common.c" "${HAP}/esp_hap_core/src/esp_hap_pairings.c"
        PROPERTIES COMPILE_OPTIONS -Wno-unused-parameter)
    amity_test(hap_http_test
        SOURCES "${HAP}/esp_hap_core/src/amity_hap_http.c" "${T}/hap_http_test.c"
        INCLUDES "${T}/hap_http_fakes" "${HAP}/esp_hap_core/src/priv_includes")
    set_target_properties(hap_http_test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    add_library(homekit_platform_httpd OBJECT "${HAP}/esp_hap_platform/src/hap_platform_httpd.c")
    target_include_directories(homekit_platform_httpd PRIVATE "${T}/homekit_platform_fakes")
    target_compile_options(homekit_platform_httpd PRIVATE -Wno-unused-function)
    target_link_libraries(homekit_platform_httpd PRIVATE strict)
    set_target_properties(homekit_platform_httpd PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    foreach(encryption plain encrypted)
        set(defines)
        if(encryption STREQUAL encrypted)
            set(defines CONFIG_NVS_ENCRYPTION=1)
        endif()
        amity_test(homekit_platform_test_${encryption} PTHREAD
            SOURCES "${HAP}/esp_hap_platform/src/hap_platform_keystore.cpp" "${T}/homekit_platform_test.cpp"
                $<TARGET_OBJECTS:homekit_platform_httpd>
            INCLUDES "${T}/homekit_platform_fakes" "${HAP}/esp_hap_platform/include"
            DEFINES ${defines})
    endforeach()
    amity_python_tests(
        homekit_http_test.py
        homekit_refresh_test.py
        homekit_worker_test.py
    )
elseif(AMITY_TEST_TIER STREQUAL sdk)
    amity_python_tests(
        homekit_crypto_test.py
        homekit_notifications_test.py
    )
    amity_host_mbedtls_python_tests(homekit_crypto_test.py)
endif()
