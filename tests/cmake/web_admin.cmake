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

    # The pairing QR codes come from the ESP-Matter checkout, as in the firmware.
    if(DEFINED ENV{ESP_MATTER_PATH})
        set(web_admin_qr_code "$ENV{ESP_MATTER_PATH}")
    else()
        set(web_admin_qr_code "$ENV{HOME}/.espressif/v6.1/esp-matter")
    endif()
    set(web_admin_qr_code "${web_admin_qr_code}/connectedhomeip/connectedhomeip/examples/common/QRCode/repo/c")
    if(NOT EXISTS "${web_admin_qr_code}/qrcodegen.c")
        message(FATAL_ERROR "${web_admin_qr_code}/qrcodegen.c is missing. Install ESP-Matter, "
            "or configure with -DAMITY_HOST_TESTS=standalone.")
    endif()
    add_library(web_admin_qrcodegen STATIC "${web_admin_qr_code}/qrcodegen.c")
    target_include_directories(web_admin_qrcodegen PRIVATE "${web_admin_qr_code}")
    target_link_libraries(web_admin_qrcodegen PRIVATE sanitized)

    set(web_admin_includes "${T}/core_fakes" "${T}/web_request_fakes" "${T}/web_admin_fakes" "${T}" "${json}"
        "${web_admin_qr_code}" "${C}/web_admin" "${C}/web_admin/include" "${C}/web" "${C}/web/include"
        "${C}/config_store/include" "${C}/device_settings/include" "${C}/system_services/include"
        "${C}/amity_matter/include" "${C}/amity_homekit/include" "${C}/ble_remote/include"
        "${C}/consortium/include" "${C}/amity_system/include" "${C}/amity_hub/include" "${C}/cec/include")
    # web_admin.cpp declares its embedded assets in its anonymous namespace,
    # and the firmware defines them outside C++, which clang reports.
    amity_host_compile(web_admin SOURCES "${C}/web_admin/web_admin.cpp" INCLUDES ${web_admin_includes})
    target_compile_options(web_admin PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-undefined-internal>)
    amity_test(web_admin_test
        SOURCES "${T}/web_admin_test.cpp" "${C}/web/request.cpp"
        INCLUDES ${web_admin_includes}
        LIBS core_fakes httpd_fake web_server_seams web_redirect_seams web_credentials_seams
            web_system_admin_seams activity_editor_seams system_services_seams log_download_seams
            device_settings_seams factory_reset_seams consortium_runtime_seams hub_service_seams
            amity_matter_seams homekit_seams ble_remote_seams amity_model cjson web_admin_qrcodegen
        REGISTRY
        SCENARIOS ready public-boundary setup-status disabled asset-headers asset-revalidation member prepared
            recovery_required unclaimed awaiting_setup stop-before-start live-stop stop-error redirect-error
            redirect-stop-error both-stop-errors route-error firmware-unauthenticated firmware-get
            firmware-upload sanity strict)
    set_tests_properties(web_admin_test.strict PROPERTIES
        PASS_REGULAR_EXPRESSION "seam device_settings::reset_pending was called without a behaviour")
    unset(web_admin_qr_code)
    unset(web_admin_includes)

    amity_python_tests(
        controller_api_surface_test.py
    )
endif()
