# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# System services: diagnostics, logs and firmware updates.
if(AMITY_TEST_TIER STREQUAL standalone)
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
        allocation_failure_diagnostics_test.py
        console_latency_observer_test.py
        log_download_test.py
        partition_staging_test.py
        update_bundle_test.py
    )
endif()
