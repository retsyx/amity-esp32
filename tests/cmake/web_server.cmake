# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Web server: requests, responses, HTTPS sessions and the redirect server.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_test(web_request_test PTHREAD
        SOURCES "${C}/web/request.cpp" "${T}/web_request_test.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_request_fakes" "${C}/web" "${C}/web/include"
            "${C}/system_services/include" "${C}/consortium/include" "${C}/amity_system/include"
        LIBS core_fakes httpd_fake)
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
