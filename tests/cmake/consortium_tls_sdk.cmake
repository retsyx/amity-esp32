# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# The sdk tier of consortium_tls.cmake: credentials.cpp and tls_link.cpp built
# whole against ESP-IDF's pinned Mbed TLS (host_mbedtls.cmake), over the NVS,
# random fill and select of tests/consortium_tls_sdk_fakes.
set(tls_includes "${T}/consortium_tls_sdk_fakes" "${T}/core_fakes" "${C}/consortium" "${C}/consortium/include"
    "${C}/amity_system/include" "${C}/config_store/include")
amity_host_compile(consortium_credentials_sdk SOURCES "${C}/consortium/credentials.cpp" INCLUDES ${tls_includes})
amity_host_compile(consortium_tls_link_sdk SOURCES "${C}/consortium/tls_link.cpp" INCLUDES ${tls_includes})
add_library(consortium_tls_sdk_fakes OBJECT "${T}/consortium_tls_sdk_fakes/platform_fakes.cpp")
target_include_directories(consortium_tls_sdk_fakes PRIVATE ${tls_includes} "${T}" "${T}/fakes")
target_compile_options(consortium_tls_sdk_fakes PRIVATE ${amity_seam_warnings})
target_link_libraries(consortium_tls_sdk_fakes PRIVATE strict)
foreach(target IN ITEMS consortium_credentials_sdk consortium_tls_link_sdk consortium_tls_sdk_fakes)
    target_link_libraries(${target} PRIVATE host_mbedtls_tls)
endforeach()
amity_test(consortium_tls_sdk_test
    SOURCES "${T}/consortium_tls_sdk_test.cpp" "${C}/consortium/setup_timing.cpp"
    INCLUDES ${tls_includes}
    LIBS consortium_credentials_sdk consortium_tls_link_sdk consortium_tls_sdk_fakes core_fakes
        config_store_nvs_seams amity_consortium amity_model host_mbedtls_tls
    REGISTRY
    SCENARIOS identity-generation identity-record certificate profile-binding verify-callback stored-roster
        setup-peers roster-capacity connection-admission mutual-tls socket-options non-tcp-socket tcp-frames
        primitives record-storage group-storage group-capacity)
unset(tls_includes)
