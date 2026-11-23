# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Consortium TLS links, credentials and group storage.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_host_compile(consortium_setup_timing
        SOURCES "${C}/consortium/setup_timing.cpp"
        INCLUDES "${T}/core_fakes" "${C}/consortium" "${C}/consortium/include" "${C}/amity_system/include")
    amity_test(consortium_setup_timing_test
        SOURCES "${T}/consortium_setup_timing_test.cpp"
        INCLUDES "${T}/core_fakes" "${C}/consortium" "${C}/consortium/include" "${C}/amity_system/include"
        LIBS consortium_setup_timing core_fakes
        REGISTRY
        SCENARIOS deferred-emission unfinished-stage line-formats worst-case-line capacity finish-once done-and-step)
    amity_python_tests(
        consortium_group_storage_test.py
    )
elseif(AMITY_TEST_TIER STREQUAL sdk)
    amity_python_tests(
        consortium_tls_credentials_test.py
        consortium_tls_tcp_test.py
    )
    # Tests that configure and build the one shared host Mbed TLS tree.
    set_tests_properties(consortium_tls_credentials_test.py consortium_tls_tcp_test.py PROPERTIES
        RESOURCE_LOCK host_mbedtls)
endif()
include("${T}/cmake/consortium_tls_link.cmake")
