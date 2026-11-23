# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Web server: requests, responses, HTTPS sessions and the redirect server.

# HTTPD's private request state, for tests in either tier that build it
# instead of linking the HTTP server fake. Tests list "${T}/web_server_fakes"
# after "${T}/core_fakes" and before "${T}/web_request_fakes".
if(NOT TARGET web_server_fakes)
    add_library(web_server_fakes OBJECT "${T}/web_server_fakes/httpd_private.cpp")
    target_include_directories(web_server_fakes PRIVATE "${T}/core_fakes" "${T}/web_server_fakes"
        "${T}/web_request_fakes" "${T}")
    target_compile_options(web_server_fakes PRIVATE ${amity_seam_warnings})
    target_link_libraries(web_server_fakes PRIVATE strict)
    # The response path builds in every tier, and links into the HTTPS
    # response test.
    amity_host_compile(web_request
        SOURCES "${C}/web/request.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include")
    amity_host_compile(web_tls_write
        SOURCES "${C}/web/tls_write.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes")
endif()

if(AMITY_TEST_TIER STREQUAL standalone)
    amity_test(web_request_test PTHREAD
        SOURCES "${C}/web/request.cpp" "${T}/web_request_test.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include"
            "${C}/system_services/include" "${C}/consortium/include" "${C}/amity_system/include"
        LIBS core_fakes httpd_fake)
    amity_host_compile(web_stall
        SOURCES "${C}/web/stall.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include")
    amity_test(web_stall_test PTHREAD RACE
        SOURCES "${T}/web_stall_test.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include"
        LIBS web_stall core_fakes web_server_fakes
        REGISTRY
        SCENARIOS before-prepare request-head unread-body handler-response other-server handshake)
    amity_host_compile(web_redirect
        SOURCES "${C}/web/redirect.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes" "${C}/web/include")
    amity_test(web_redirect_test PTHREAD RACE
        SOURCES "${T}/web_redirect_test.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_request_fakes" "${C}/web/include"
        LIBS web_redirect core_fakes
        REGISTRY
        SCENARIOS location start-once task-failure head-in-pieces oversized-head unread-body drained-body
            stalled-client replacement stop-and-restart)
    amity_host_compile(web_server
        SOURCES "${C}/web/web.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include")
    amity_test(web_server_test
        SOURCES "${T}/web_server_test.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include"
        LIBS core_fakes httpd_fake web_credentials_seams
        REGISTRY
        SCENARIOS start-order start-configuration start-failures handshake handshake-option-failure stop route)
    amity_python_tests(
        web_redirect_test.py
        web_session_tcp_test.py
    )
elseif(AMITY_TEST_TIER STREQUAL sdk)
    # The pinned SDK functions the response path runs on, cut with #line
    # directives that name their SDK files.
    set(web_idf "$ENV{IDF_PATH}")
    if(NOT web_idf)
        set(web_idf "$ENV{HOME}/.espressif/v6.1/esp-idf")
    endif()
    set(web_sdk_functions "${CMAKE_CURRENT_BINARY_DIR}/https_response_write_sdk.c")
    add_custom_command(OUTPUT "${web_sdk_functions}"
        COMMAND "${PYTHON3}" "${T}/extract_sdk_functions.py" "${T}/https_response_write_sdk.json"
            "${web_sdk_functions}" --idf "${web_idf}"
        DEPENDS "${T}/extract_sdk_functions.py" "${T}/https_response_write_sdk.json"
            "${web_idf}/components/mbedtls/port/aes/dma/esp_aes_dma_core.c"
            "${web_idf}/components/esp-tls/esp_tls_mbedtls.c" "${web_idf}/components/esp-tls/esp_tls.c"
            "${web_idf}/components/esp_http_server/src/httpd_txrx.c"
        VERBATIM)
    # The SDK compares signed results with sizes.
    set_source_files_properties("${web_sdk_functions}" PROPERTIES COMPILE_OPTIONS -Wno-sign-compare)
    amity_test(https_response_write_test
        SOURCES "${T}/https_response_write_test.cpp" "${web_sdk_functions}"
        INCLUDES "${T}/core_fakes" "${T}/web_server_fakes" "${T}/web_request_fakes" "${T}/crypto_fakes" "${T}"
            "${C}/web" "${C}/web/include"
        LIBS web_request web_tls_write core_fakes web_server_fakes
        REGISTRY
        SCENARIOS bounded-records body-sizes short-and-want-writes write-errors zero-progress dma-failure
            bounded-write)
    unset(web_idf)
    unset(web_sdk_functions)
    # The HTTPS identity, built whole against ESP-IDF's pinned Mbed TLS. Its
    # certificates are also parsed by OpenSSL.
    set(web_tls_identity_includes "${T}/core_fakes" "${C}/web" "${C}/config_store/include")
    amity_host_compile(web_tls_identity SOURCES "${C}/web/tls_identity.cpp" INCLUDES ${web_tls_identity_includes})
    target_link_libraries(web_tls_identity PRIVATE host_mbedtls_x509)
    amity_test(web_tls_identity_test
        SOURCES "${T}/web_tls_identity_test.cpp"
        INCLUDES ${web_tls_identity_includes}
        LIBS web_tls_identity host_mbedtls_x509 OpenSSL::Crypto
        REGISTRY
        SCENARIOS redundant-zero-draw masked-draw necessary-zero-draw all-ones-draw all-zero-draw
            repair-redundant-zero repair-zero-serial repair-negative-serial repair-garbage preserve-necessary-zero
            missing-key missing-certificate certificate-read-error key-read-error key-write-error
            certificate-write-error hostname-subject key-released)
    unset(web_tls_identity_includes)
    amity_python_tests(
        https_response_write_test.py
        web_stall_test.py
    )
endif()
