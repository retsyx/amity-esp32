# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Web administration pages, their scripts and the API behind them.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_python_tests(
        original_controller_web_test.py
    )
    amity_node_tests(
        activity_choice_identity_ui_test.js
        firmware_version_test.js
        hostname_reconnect_ui_test.js
        log_download_ui_test.js
        membership_admission_ui_test.js
        membership_removal_ui_test.js
        poll_recovery_ui_test.js
    )
    amity_command_test(app_js_syntax "${NODE}" --check "${C}/web_admin/assets/app.js")
    amity_command_test(system_js_syntax "${NODE}" --check "${C}/web_admin/assets/system.js")
elseif(AMITY_TEST_TIER STREQUAL sdk)
    amity_test(json_input_test
        SOURCES "${T}/json_input_test.cpp"
        INCLUDES "${json}" "${C}/web_admin/include"
        LIBS cjson)
    amity_test(local_remote_approval_test
        SOURCES "${T}/local_remote_approval_test.cpp"
        INCLUDES "${json}" "${C}/web_admin" "${C}/web_admin/include" "${C}/amity_system/include"
        LIBS amity_model cjson)
    amity_python_tests(
        controller_api_surface_test.py
        hostname_reconnect_api_test.py
    )
endif()
