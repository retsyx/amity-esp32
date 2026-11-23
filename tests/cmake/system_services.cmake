# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# System services: diagnostics, logs and firmware updates.
if(AMITY_TEST_TIER STREQUAL standalone)
    # The SDK calls of system_services.cpp and log_download.cpp beyond the core
    # fakes, as strict seams.
    add_library(system_services_fakes OBJECT "${T}/system_services_fakes/system_services_fakes.cpp")
    target_include_directories(system_services_fakes PRIVATE "${T}/system_services_fakes" "${T}/core_fakes" "${T}")
    target_compile_options(system_services_fakes PRIVATE ${amity_seam_warnings})
    target_link_libraries(system_services_fakes PRIVATE strict)
    set(system_services_includes "${T}/core_fakes" "${T}/system_services_fakes" "${T}/web_request_fakes"
        "${T}/crypto_fakes" "${C}/system_services/include" "${C}/consortium/include" "${C}/amity_system/include"
        "${C}/device_settings/include" "${C}/network_diagnostics/include" "${C}/web/include")
    amity_host_compile(system_services_cpp
        SOURCES "${C}/system_services/system_services.cpp"
        INCLUDES ${system_services_includes})
    amity_host_compile(log_download_cpp
        SOURCES "${C}/system_services/log_download.cpp"
        INCLUDES ${system_services_includes})
    # Writes a bundle made by the release tool, which python-bundle uploads.
    amity_command_test(update_bundle_fixture "${PYTHON3}" -S "${T}/update_bundle_fixture.py"
        "${CMAKE_BINARY_DIR}/python-update-bundle.bin")
    set_tests_properties(update_bundle_fixture PROPERTIES FIXTURES_SETUP python_update_bundle)
    amity_test(system_services_test
        SOURCES "${T}/system_services_test.cpp"
        INCLUDES ${system_services_includes}
        LIBS system_services_fakes core_fakes httpd_fake web_request_seams network_diagnostics_seams
            firmware_update_seams peer_firmware_update_seams group_firmware_update_seams amity_model
        DEFINES "AMITY_PYTHON_UPDATE_BUNDLE=\"${CMAKE_BINARY_DIR}/python-update-bundle.bin\""
        REGISTRY
        SCENARIOS log-capture-start log-capture-without-lock allocation-hook allocation-capacity allocation-names
            allocation-in-interrupt memory-totals console-forwarding console-long-line console-error
            console-without-sink console-clock-wrap console-clock-anomaly bundle-upload raw-image bundle-corruptions
            bundle-length receive-failures signed-bundle image-digest header-bounds metadata-trickle image-stall
            reader-unavailable downgrade upload-rejections python-bundle confirm-pending-image
            confirm-confirmed-image reboot-later log-ring)
    set_tests_properties(system_services_test.python-bundle PROPERTIES FIXTURES_REQUIRED python_update_bundle)
    amity_test(log_download_test PTHREAD RACE
        SOURCES "${T}/log_download_test.cpp"
        INCLUDES ${system_services_includes}
        LIBS system_services_cpp system_services_fakes core_fakes httpd_fake consortium_runtime_seams
            device_settings_seams web_request_seams network_diagnostics_seams firmware_update_seams
            peer_firmware_update_seams group_firmware_update_seams amity_model
        REGISTRY
        SCENARIOS setup-failures snapshot main-download http-failure member-send-failure member-download
            local-memory slow-member stop-during-refusal)
    amity_test(firmware_version_test
        SOURCES "${T}/firmware_version_test.cpp"
        INCLUDES "${C}/system_services/include"
        ARGS "${T}/firmware_versions.tsv")
    amity_command_test(release_manifest_fixture "${PYTHON3}" -S "${T}/release_manifest_fixture.py"
        "${CMAKE_BINARY_DIR}/release-manifest.json")
    set_tests_properties(release_manifest_fixture PROPERTIES FIXTURES_SETUP release_manifest)
    amity_test(release_manifest_test
        SOURCES "${C}/system_services/release_manifest.cpp" "${T}/release_manifest_test.cpp"
        INCLUDES "${C}/consortium/include" "${C}/amity_system/include" "${C}/system_services/include"
        LIBS amity_model amity_consortium
        ARGS "${CMAKE_BINARY_DIR}/release-manifest.json"
        FIXTURES release_manifest)
    amity_test(cohort_update_test
        SOURCES "${C}/system_services/group_firmware_update.cpp" "${C}/system_services/release_manifest.cpp"
            "${T}/cohort_update_test.cpp"
        INCLUDES "${T}/cohort_fakes" "${T}/cec_hub_fakes" "${T}/reset_fakes" "${C}/consortium/include"
            "${C}/amity_system/include" "${C}/config_store/include" "${C}/system_services/include"
        LIBS amity_model amity_consortium OpenSSL::Crypto
        ARGS "${CMAKE_BINARY_DIR}/release-manifest.json"
        FIXTURES release_manifest)
    amity_test(firmware_maintenance_test
        SOURCES "${C}/system_services/firmware_update.cpp" "${C}/system_services/peer_firmware_update.cpp"
            "${T}/firmware_maintenance_test.cpp"
        INCLUDES "${T}/firmware_maintenance_fakes" "${C}/system_services/include" "${C}/config_store/include"
            "${C}/amity_system/include")
    amity_python_tests(
        partition_staging_test.py
        update_bundle_test.py
    )
endif()
