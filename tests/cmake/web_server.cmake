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
    amity_python_tests(
        web_redirect_test.py
        web_session_tcp_test.py
    )
elseif(AMITY_TEST_TIER STREQUAL sdk)
    amity_python_tests(
        https_response_write_test.py
        web_stall_test.py
        web_tls_identity_test.py
    )
    amity_host_mbedtls_python_tests(web_tls_identity_test.py)
endif()
