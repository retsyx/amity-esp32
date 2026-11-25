# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# System administration API.
if(AMITY_TEST_TIER STREQUAL sdk)
    set(system_admin_includes "${T}/core_fakes" "${T}/web_request_fakes" "${T}/system_admin_fakes"
        "${T}/device_settings_fakes" "${json}" "${C}/web_admin/include" "${C}/web" "${C}/web/include"
        "${C}/config_store/include" "${C}/system_services/include" "${C}/device_settings/include"
        "${C}/consortium/include" "${C}/amity_system/include" "${C}/system_sync/include" "${C}/amity_hub/include"
        "${C}/cec/include")
    amity_host_compile(system_admin
        SOURCES "${C}/web_admin/system_admin.cpp"
        INCLUDES ${system_admin_includes})
    # serialize_configuration is renamed in this copy, so the harness's
    # definition records each serialization made by another file.
    amity_host_compile(system_admin_configuration
        SOURCES "${C}/config_store/system_configuration.cpp"
        INCLUDES ${system_admin_includes}
        DEFINES serialize_configuration=actual_serialize_configuration)
    target_link_libraries(system_admin_configuration PRIVATE OpenSSL::Crypto)
    # The activity editor, over the same control, hub and runtime seams. Its
    # Wi-Fi read uses the HomeKit fake header, after tests/fakes.
    set(activity_editor_includes ${system_admin_includes} "${C}/web_admin" "${T}" "${T}/fakes" "${T}/homekit_fakes")
    amity_host_compile(system_admin_activity_editor
        SOURCES "${C}/web_admin/activity_editor.cpp" "${C}/web_admin/activity_editor_model.cpp"
        INCLUDES ${activity_editor_includes})
    add_library(activity_editor_seams_for_admin OBJECT "${T}/system_admin_fakes/activity_editor_seams.cpp")
    target_include_directories(activity_editor_seams_for_admin PRIVATE ${activity_editor_includes})
    target_compile_options(activity_editor_seams_for_admin PRIVATE ${amity_seam_warnings})
    target_link_libraries(activity_editor_seams_for_admin PRIVATE strict)
    add_library(system_admin_seams OBJECT "${T}/system_admin_fakes/system_admin_seams.cpp")
    target_include_directories(system_admin_seams PRIVATE ${system_admin_includes} "${T}" "${T}/fakes")
    target_compile_options(system_admin_seams PRIVATE ${amity_seam_warnings})
    target_link_libraries(system_admin_seams PRIVATE strict)
    amity_test(system_admin_test
        SOURCES "${T}/system_admin_test.cpp" "${C}/web/request.cpp" "${C}/device_settings/device_settings.cpp"
            "${C}/system_sync/system_sync.cpp" "${C}/amity_hub/system_control.cpp" "${C}/amity_hub/remote_control.cpp"
            "${C}/config_store/configuration_journal.cpp" "${C}/config_store/configuration_replacement.cpp"
        INCLUDES ${system_admin_includes} "${T}" "${C}/web_admin" "${T}/fakes" "${T}/homekit_fakes"
        LIBS system_admin_configuration system_admin_seams system_admin_activity_editor activity_editor_seams_for_admin consortium_runtime_seams system_services_seams
            adapter_settings_seams group_firmware_update_seams config_store_nvs_seams hub_service_seams core_fakes
            httpd_fake amity_model amity_consortium cjson OpenSSL::Crypto
        REGISTRY
        SCENARIOS error-response rejection-before-body retained retained-pending-image hardware-change
            initial-configuration pending-image pending-image-healthy pending-initial-healthy reservation-failure
            commit-failure read-failure foreign-controller deferred-load passive-states config-read config-unavailable
            admission-contract admission-start admission-completes admission-wrong-name admission-scan-change
            admission-failure admission-status scan-refusals removal-review removal-reservation-failure
            removal-update-required removal-marker-failure removal-success removal-forget-failure removal-saved-only
            removal-metadata-only replacement replacement-review request-fields administrative-paths unknown-change
            update-cohort system-snapshot system-peers editor-status-scan-order sanity)
endif()
