# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Tests that belong to no other area.
if(AMITY_TEST_TIER STREQUAL standalone)
    # Storage and device settings
    amity_test(config_store_test
        SOURCES "${C}/config_store/config_store.cpp" "${T}/config_store_test.cpp"
        INCLUDES "${T}/reset_fakes" "${C}/config_store/include")
    amity_test(identity_journal_test
        SOURCES "${T}/identity_journal_test.cpp"
        INCLUDES "${C}/config_store/include" "${C}/amity_system/include")
    amity_test(reset_button_test
        SOURCES "${C}/device_settings/reset_button.cpp" "${T}/reset_button_test.cpp"
        INCLUDES "${T}/reset_fakes" "${C}/amity_system/include" "${C}/device_settings/include")
    amity_test(reset_button_interrupt_test
        SOURCES "${C}/device_settings/reset_button.cpp" "${T}/reset_button_interrupt_test.cpp"
        INCLUDES "${T}/reset_button_fakes" "${T}/reset_fakes" "${C}/amity_system/include"
            "${C}/device_settings/include"
        SCENARIOS idle boot-held hold short cancel boot-release-hold latched-edge retry
            allocation-failure shared-service-failure handler-failure setup-short setup-before-three-seconds
            setup-three-seconds setup-release-bounce setup-press-bounce-short setup-press-bounce-three-seconds
            setup-before-reset setup-reset-release setup-after-reset setup-long setup-boot-held)
    amity_test(physical_address_test
        SOURCES "${T}/physical_address_test.cpp"
        INCLUDES "${C}/amity_system/include" "${C}/device_settings/include")
    amity_test(device_settings_test
        SOURCES "${C}/device_settings/device_settings.cpp" "${T}/device_settings_test.cpp"
        INCLUDES "${T}/device_settings_fakes" "${T}/reset_fakes" "${T}/cec_hub_fakes" "${C}/amity_system/include"
            "${C}/device_settings/include" "${C}/config_store/include")
    amity_test(adapter_settings_test
        SOURCES "${C}/system_services/adapter_settings.cpp" "${C}/device_settings/device_settings.cpp"
            "${T}/adapter_settings_test.cpp"
        INCLUDES "${T}/adapter_settings_fakes" "${T}/device_settings_fakes" "${T}/reset_fakes" "${T}/cec_hub_fakes"
            "${T}/cohort_fakes" "${C}/system_services/include" "${C}/device_settings/include"
            "${C}/config_store/include" "${C}/consortium/include" "${C}/amity_system/include"
        LIBS amity_model amity_consortium)
    amity_test(member_removal_reset_test
        SOURCES "${C}/system_services/member_removal.cpp" "${C}/device_settings/factory_reset.cpp"
            "${T}/member_removal_reset_test.cpp"
        INCLUDES "${T}/member_removal_fakes" "${T}/reset_fakes" "${T}/cohort_fakes" "${C}/system_services/include"
            "${C}/device_settings/include" "${C}/config_store/include" "${C}/amity_system/include"
            "${C}/consortium/include"
        LIBS amity_model)
    amity_test(factory_credentials_test
        SOURCES "${T}/factory_credentials_test.cpp"
        INCLUDES "${C}/amity_system/include" "${C}/device_settings/include"
        ARGS "${ROOT}/amity-provision/testdata/factory-v3.hex")
    amity_test(factory_reset_test
        SOURCES "${C}/device_settings/factory_reset.cpp" "${T}/factory_reset_test.cpp"
        INCLUDES "${T}/reset_fakes" "${C}/amity_system/include" "${C}/device_settings/include")
    amity_python_tests(
        board_profile_config_test.py
        hardware_capacity_test.py
    )

    # Activities, HDMI-CEC and the hub
    set(hub_includes "${T}/cec_hub_fakes" "${C}/amity_system/include" "${C}/config_store/include"
        "${C}/cec/include" "${C}/amity_hub/include")
    amity_test(cec_hub_test
        SOURCES "${C}/cec/engine.cpp" "${C}/amity_hub/hub.cpp" "${T}/cec_hub_test.cpp"
        INCLUDES "${C}/amity_system/include" "${C}/config_store/include" "${C}/cec/include"
            "${C}/amity_hub/include")
    foreach(test cec_hub_commit_test cec_front_physical_test cec_hub_settling_test owned_hub_test
            owned_configuration_revision_test cec_scan_policy_test owned_source_theft_test
            owned_hub_settling_test device_announcement_test native_ports_test)
        amity_test(${test}
            SOURCES "${C}/amity_hub/hub.cpp" "${C}/amity_hub/hub_service.cpp" "${T}/${test}.cpp"
            INCLUDES ${hub_includes}
            LIBS amity_model)
    endforeach()
    amity_test(local_activity_step_test
        SOURCES "${C}/amity_hub/hub.cpp" "${C}/amity_hub/hub_service.cpp" "${T}/local_activity_step_test.cpp"
        INCLUDES ${hub_includes}
        LIBS amity_model
        SCENARIOS settings-commit ipc-full ipc-slots fifo-full-drop display-other-adapter source-other-adapter reaction-frame
            authority-loss authority-kept lookup-waiter-first lookup-step-first route-commit audio-key-released audio-key-held
            audio-key-held-deferred context-lookup-keys context-lookup-repair context-lookup-selection)
    amity_test(main_reboot_member_test
        SOURCES "${C}/amity_hub/hub.cpp" "${C}/amity_hub/hub_service.cpp" "${T}/main_reboot_member_test.cpp"
        INCLUDES ${hub_includes}
        LIBS amity_model
        SCENARIOS one-adapter one-adapter-hold two-adapters unreferenced-adapter roster-only reselect-after-forced-standby
            member-settings shared-after-forced-standby reboot-mid-recipe standby-after-reboot unconfigured-context dropped-equipment
            outage-keeps-activity release-before-adoption)
    amity_test(activity_sequence_test
        SOURCES "${C}/amity_hub/hub.cpp" "${C}/amity_hub/hub_service.cpp" "${C}/ble_remote/remote_input.cpp"
            "${C}/ble_remote/siri_remote.cpp" "${T}/activity_sequence_test.cpp"
        INCLUDES ${hub_includes} "${C}/ble_remote/include"
        LIBS amity_model)
    amity_test(cec_activity_sequence_test
        SOURCES "${C}/amity_hub/hub.cpp" "${T}/cec_activity_sequence_test.cpp"
        INCLUDES "${T}/cec_hub_fakes" "${C}/amity_system/include" "${C}/cec/include" "${C}/amity_hub/include"
        LIBS amity_model)
    amity_test(addressing_test
        SOURCES "${T}/addressing_test.cpp"
        INCLUDES "${C}/amity_system/include"
        LIBS amity_model)
    amity_test(cec_runtime_test
        SOURCES "${C}/cec/engine.cpp" "${C}/cec/scheduler.cpp" "${T}/cec_runtime_test.cpp"
        INCLUDES "${C}/amity_system/include" "${C}/cec/include")
    amity_test(cec_runtime_driver_test
        SOURCES "${C}/cec/engine.cpp" "${C}/cec/scheduler.cpp" "${C}/cec/bus.cpp" "${T}/cec_runtime_driver_test.cpp"
        INCLUDES "${T}/cec_runtime_fakes" "${C}/amity_system/include" "${C}/cec/include")

    # Bluetooth remotes
    amity_test(ble_keyboard_test
        SOURCES "${C}/ble_remote/hid_keyboard.cpp" "${C}/ble_remote/atomic_remote.cpp"
            "${C}/ble_remote/remote_input.cpp" "${T}/ble_keyboard_test.cpp"
        INCLUDES "${C}/ble_remote/include")
    amity_test(ble_controller_test
        SOURCES "${C}/ble_remote/atomic_remote.cpp" "${C}/ble_remote/remote_input.cpp" "${T}/ble_controller_test.cpp"
        INCLUDES "${C}/ble_remote/include")
    amity_test(siri_remote_test
        SOURCES "${C}/ble_remote/siri_remote.cpp" "${C}/ble_remote/remote_input.cpp" "${T}/siri_remote_test.cpp"
        INCLUDES "${C}/ble_remote/include")
    amity_test(remote_input_test
        SOURCES "${T}/remote_input_test.cpp" "${C}/ble_remote/atomic_remote.cpp" "${C}/ble_remote/siri_remote.cpp"
            "${C}/ble_remote/remote_input.cpp"
        INCLUDES "${C}/ble_remote/include")
    amity_test(key_sequence_test SOURCES "${T}/key_sequence_test.cpp" INCLUDES "${ROOT}/main")
    amity_test(pairing_policy_test SOURCES "${T}/pairing_policy_test.cpp" INCLUDES "${C}/ble_remote/include")
    amity_test(gatt_layout_test
        SOURCES "${C}/ble_remote/gatt_layout.cpp" "${T}/gatt_layout_test.cpp"
        INCLUDES "${C}/ble_remote/include")
    amity_test(battery_status_test SOURCES "${T}/battery_status_test.cpp" INCLUDES "${C}/ble_remote/include")
    set(ble_transport_sources "${T}/ble_transport_test.cpp" "${C}/ble_remote/hid_keyboard.cpp"
        "${C}/ble_remote/siri_remote.cpp" "${C}/ble_remote/remote_input.cpp" "${C}/ble_remote/gatt_layout.cpp")
    amity_test(ble_transport_test
        SOURCES ${ble_transport_sources}
        INCLUDES "${T}/ble_transport_fakes" "${C}/ble_remote/include"
        SCENARIOS unpair-privacy unpair-privacy-cancel unpair-privacy-error unpair-privacy-connected
            pairing-cleanup-privacy pairing-cleanup-privacy-error pairing-disconnected-privacy
            pairing-disconnected-before-security pairing-resolved-identity-error pairing-resolved-random-error
            pairing-cleanup-retry pairing-retry-establishment pairing-retry-supervision pairing-retry-unencrypted
            pairing-retry-exhausted pairing-retry-refusal pairing-retry-deadline pairing-retry-search-end
            pairing-retry-late-remote pairing-retry-connect-timeout pairing-retry-setup-failure
            pairing-retry-cleanup-failure
            absence success unpair unpair-delete-error unpair-storage-error
            unpair-before-completion unpair-pairing unpair-cancel-error connect-error async-error
            discovery-error disconnect-before-setup unreported-link-lost reconnect-backoff host-reset pairing
            stop-pairing-search stop-pairing-connect stop-pairing-security saved-layout disconnect-held-key
            changed-layout lost-layout-validation layout-without-hash)
    amity_test(ble_transport_no_privacy_test
        SOURCES ${ble_transport_sources}
        INCLUDES "${T}/ble_transport_fakes" "${C}/ble_remote/include"
        DEFINES MYNEWT_VAL_BLE_HS_PVCY=0
        SCENARIOS unpair-no-privacy)

    # Release tooling
    amity_python_tests(
        release_hardware_test.py
        release_scripts_test.py
    )

    # Setup tool
    amity_command_test(provisioning_go_test "${GO}" test ./...)
    amity_command_test(provisioning_go_vet "${GO}" vet ./...)
    amity_command_test(provisioning_tool_build "${GO}" build -o "${CMAKE_BINARY_DIR}/bin/amity-provision-test" .)
    set_tests_properties(provisioning_go_test provisioning_go_vet provisioning_tool_build PROPERTIES
        WORKING_DIRECTORY "${ROOT}/amity-provision"
        ENVIRONMENT "GOCACHE=${ROOT}/.cache/go-build;GOMODCACHE=${ROOT}/.cache/go-mod")
    set_tests_properties(provisioning_tool_build PROPERTIES FIXTURES_SETUP provisioning_tool)
    # The terminal prompt needs a pseudo-terminal, which the Go tests cannot open.
    amity_command_test(provisioning_prompt.py "${PYTHON3}" -S "${T}/provisioning_prompt.py"
        "${CMAKE_BINARY_DIR}/bin/amity-provision-test")
    set_tests_properties(provisioning_prompt.py PROPERTIES FIXTURES_REQUIRED provisioning_tool)

    # Network diagnostics
    amity_python_tests(
        wifi_signal_test.py
    )

    # Scenario test support. The WILL_FAIL tests are the cases that must fail.
    amity_test(scenario_main_test PTHREAD RACE
        SOURCES "${T}/scenario_main_test.cpp"
        REGISTRY
        SCENARIOS checks threads no-checks failed-check)
    amity_command_test(scenario_main_test.unknown-scenario scenario_main_test unknown)
    amity_command_test(scenario_main_test.unregistered-scenario
        scenario_main_test --verify-scenarios checks threads no-checks)
    amity_command_test(scenario_main_test.missing-scenario
        scenario_main_test --verify-scenarios checks threads no-checks failed-check missing)
    set_tests_properties(scenario_main_test.no-checks scenario_main_test.failed-check
        scenario_main_test.unknown-scenario scenario_main_test.unregistered-scenario
        scenario_main_test.missing-scenario PROPERTIES WILL_FAIL TRUE)

    # Shared SDK fakes. The scenarios with a pass expression are the cases that must fail.
    amity_test(core_fakes_test PTHREAD RACE
        SOURCES "${T}/core_fakes_test.cpp"
        INCLUDES "${T}/core_fakes"
        LIBS core_fakes
        REGISTRY
        SCENARIOS clock-timers periodic-timer timer-failures delay-leave task-records task-return
            task-caps-mismatch static-queue queue-timeouts semaphores mutex-ownership timer-wakes-wait
            blocking-threads endless-wait notifications logs restart-and-checks heap strict-seam
            seam-behaviour critical-sections app-description)
    set_tests_properties(core_fakes_test.task-return PROPERTIES
        PASS_REGULAR_EXPRESSION "task returns returned from its function without deleting itself")
    set_tests_properties(core_fakes_test.task-caps-mismatch PROPERTIES
        PASS_REGULAR_EXPRESSION "task caps was created with capabilities but deleted with vTaskDelete")
    set_tests_properties(core_fakes_test.endless-wait PROPERTIES
        PASS_REGULAR_EXPRESSION "an endless wait on a semaphore that nothing in this thread can end")
    set_tests_properties(core_fakes_test.strict-seam PROPERTIES
        PASS_REGULAR_EXPRESSION "seam heap_caps_get_free_size was called without a behaviour")
    amity_test(httpd_fake_test
        SOURCES "${T}/httpd_fake_test.cpp"
        INCLUDES "${T}/core_fakes" "${T}/web_request_fakes"
        LIBS httpd_fake
        REGISTRY
        SCENARIOS request-reads header-lookup response-head chunked send-after-send asynchronous
            complete-original handlers wildcard-seam)
    set_tests_properties(httpd_fake_test.send-after-send PROPERTIES
        PASS_REGULAR_EXPRESSION "httpd_resp_send after the response head was sent")
    set_tests_properties(httpd_fake_test.complete-original PROPERTIES
        PASS_REGULAR_EXPRESSION "httpd_req_async_handler_complete of a request that is not an asynchronous copy")

    # Python tests that read code from several areas
    amity_python_tests(
        controller_integration_test.py
        hostname_services_test.py
        matter_input_names_test.py
        matter_state_publication_test.py
    )

    # Python tests that read firmware sources
    amity_python_tests(
        slicing_ratchet_test.py
    )
elseif(AMITY_TEST_TIER STREQUAL sdk)
    # Configuration, synchronization and the system remote control
    set(configuration_includes "${json}" "${T}/reset_fakes" "${C}/config_store/include" "${C}/amity_system/include")
    set(configuration_sources "${C}/config_store/system_configuration.cpp"
        "${C}/config_store/configuration_journal.cpp")
    foreach(test system_configuration_test configuration_replacement_test activity_editor_test)
        set(sources ${configuration_sources} "${C}/config_store/configuration_replacement.cpp" "${T}/${test}.cpp")
        if(test STREQUAL activity_editor_test)
            list(APPEND sources "${C}/web_admin/activity_editor_model.cpp")
        endif()
        set(arguments)
        if(test STREQUAL system_configuration_test)
            set(arguments ARGS "${ROOT}/example-config.json")
        endif()
        amity_test(${test}
            SOURCES ${sources}
            INCLUDES ${configuration_includes} "${C}/system_sync/include" "${C}/consortium/include"
            LIBS amity_model cjson OpenSSL::Crypto
            ${arguments})
    endforeach()
    # Rename only this translation unit's digest entry point; the test wrapper
    # calls the real SHA256 whenever the selected failure is not reached.
    add_library(configuration_failure_storage OBJECT "${C}/config_store/system_configuration.cpp")
    target_include_directories(configuration_failure_storage PRIVATE ${configuration_includes} "${T}/fakes")
    target_compile_definitions(configuration_failure_storage PRIVATE SHA256=amity_failure_sha256)
    target_link_libraries(configuration_failure_storage PRIVATE strict OpenSSL::Crypto)
    amity_test(configuration_failure_test
        SOURCES "${C}/config_store/configuration_journal.cpp" "${T}/configuration_failure_test.cpp"
            $<TARGET_OBJECTS:configuration_failure_storage>
        INCLUDES ${configuration_includes}
        LIBS amity_model cjson OpenSSL::Crypto)
    amity_test(system_sync_protocol_test PTHREAD
        SOURCES ${configuration_sources} "${T}/system_sync_protocol_test.cpp"
        INCLUDES "${json}" "${T}/system_sync_fakes" "${T}/cec_hub_fakes" "${T}/reset_fakes"
            "${C}/config_store/include" "${C}/amity_system/include" "${C}/system_sync/include"
            "${C}/consortium/include"
        LIBS amity_model amity_consortium cjson OpenSSL::Crypto)
    amity_test(system_remote_control_test
        SOURCES "${C}/amity_hub/hub.cpp" "${C}/amity_hub/hub_service.cpp" "${C}/amity_hub/system_control.cpp"
            "${C}/amity_hub/remote_control.cpp" ${configuration_sources} "${T}/system_remote_control_test.cpp"
        INCLUDES "${json}" "${T}/cec_hub_fakes" "${C}/config_store/include" "${C}/cec/include"
            "${C}/amity_hub/include" "${C}/amity_system/include" "${C}/web_admin" "${C}/web_admin/include"
        DEFINES SHA256=amity_test_sha256
        LIBS amity_model cjson OpenSSL::Crypto)
    amity_python_tests(
        json_memory_test.py
        membership_capacity_api_test.py
        production_profile_test.py
        system_nvs_budget_test.py
        system_peer_record_test.py
    )
endif()
