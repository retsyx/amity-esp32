# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Consortium GATT transport and the admission exchange it carries.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_host_compile(consortium_admission_crypto
        SOURCES "${C}/consortium/admission_crypto.cpp"
        INCLUDES "${T}/crypto_fakes" "${C}/consortium")
    amity_test(consortium_admission_crypto_test
        SOURCES "${T}/consortium_admission_crypto_test.cpp"
        INCLUDES "${T}/crypto_fakes" "${C}/consortium"
        LIBS consortium_admission_crypto crypto_fakes
        REGISTRY
        SCENARIOS round-trip replay key-generation-failure reflected-direction skipped-sequence tampered-ciphertext
            tampered-tag malformed-header cross-session edited-transcript tampered-confirmation invalid-server-id
            one-handshake record-size-limit repeated-finish)

    # The fake NimBLE host and CHIP BLE manager that gatt_transport.cpp builds
    # on, and strict seams for the admission server it drives.
    set(gatt_includes "${T}/consortium_gatt_fakes" "${T}/core_fakes" "${T}/crypto_fakes" "${C}/consortium"
        "${C}/consortium/include" "${C}/amity_system/include")
    add_library(consortium_gatt_fakes OBJECT "${T}/consortium_gatt_fakes/gatt_fakes.cpp")
    target_include_directories(consortium_gatt_fakes PRIVATE "${T}/consortium_gatt_fakes" "${T}/core_fakes" "${T}")
    target_compile_options(consortium_gatt_fakes PRIVATE ${amity_seam_warnings})
    target_link_libraries(consortium_gatt_fakes PRIVATE strict)
    add_library(consortium_admission_server_seams OBJECT "${T}/consortium_gatt_fakes/admission_server_seams.cpp")
    target_include_directories(consortium_admission_server_seams PRIVATE "${C}/consortium" "${T}")
    target_compile_options(consortium_admission_server_seams PRIVATE ${amity_seam_warnings})
    target_link_libraries(consortium_admission_server_seams PRIVATE strict)
    amity_host_compile(consortium_gatt_transport
        SOURCES "${C}/consortium/gatt_transport.cpp"
        INCLUDES ${gatt_includes})

    amity_test(consortium_gatt_callback_test
        SOURCES "${T}/consortium_gatt_callback_test.cpp"
        INCLUDES ${gatt_includes} "${T}"
        LIBS consortium_gatt_fakes consortium_admission_server_seams consortium_credentials_seams core_fakes
            amity_consortium amity_model
        PTHREAD
        REGISTRY
        SCENARIOS collect-candidates filter-and-bound callback-cookie disconnect-cancels own-disconnect
            mtu-before-discovery bounded-stage-waits missing-attributes mtu-already-sent mtu-failures
            mtu-callback-fences mtu-event-fences readvertise seal-prepared seal-stops-advertising stage-timing
            progressive-selection scan-stop-failure scan-natural-end scan-window-bounds cookie-exhaustion)
    unset(gatt_includes)
    amity_python_tests(
        consortium_gatt_admission_test.py
        consortium_gatt_fragment_test.py
    )
endif()
