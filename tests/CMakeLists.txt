# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Host tests: sanitized C/C++ tests built against firmware sources and fakes,
# plus the Python, Node and Go tests, all run by CTest. Use run_host_tests.sh.
cmake_minimum_required(VERSION 3.22)
project(amity_host_tests LANGUAGES C CXX)

# standalone: tests that need no ESP-IDF checkout, managed components or firmware
# build output. sdk: only the tests that need them. all: both.
set(AMITY_HOST_TESTS all CACHE STRING "Host tests to define: all, standalone or sdk")
set_property(CACHE AMITY_HOST_TESTS PROPERTY STRINGS all standalone sdk)
if(NOT AMITY_HOST_TESTS MATCHES "^(all|standalone|sdk)$")
    message(FATAL_ERROR "AMITY_HOST_TESTS must be all, standalone or sdk")
endif()

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(T "${ROOT}/tests")
set(C "${ROOT}/components")
set(HAP "${ROOT}/third_party/esp-homekit-sdk/components")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# run_host_tests.sh deletes bin/ before each build, so a test whose build
# fails cannot run a stale executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_program(PYTHON3 python3 REQUIRED)
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

enable_testing()

add_library(sanitized INTERFACE)
target_compile_options(sanitized INTERFACE -g -fsanitize=address,undefined)
target_link_options(sanitized INTERFACE -fsanitize=address,undefined)
add_library(strict INTERFACE)
target_compile_options(strict INTERFACE -Wall -Wextra -Werror)
target_link_libraries(strict INTERFACE sanitized)

# Firmware sources shared by many tests, compiled once.
add_library(amity_model STATIC "${C}/amity_system/model.cpp")
target_include_directories(amity_model PRIVATE "${C}/amity_system/include")
target_link_libraries(amity_model PRIVATE strict)

# The host-compilable consortium codecs. The other consortium sources are
# ESP_PLATFORM-only; a static library links only the objects a test uses.
add_library(amity_consortium STATIC
    "${C}/consortium/bootstrap.cpp" "${C}/consortium/enrollment.cpp" "${C}/consortium/json.cpp"
    "${C}/consortium/owned_intent.cpp" "${C}/consortium/radio.cpp" "${C}/consortium/session.cpp"
    "${C}/consortium/settings.cpp" "${C}/consortium/update.cpp" "${C}/consortium/wire.cpp")
target_include_directories(amity_consortium PRIVATE "${C}/consortium/include" "${C}/amity_system/include")
target_link_libraries(amity_consortium PRIVATE strict)

set(AMITY_TEST_LABEL standalone)

# amity_test(<name> SOURCES ... [INCLUDES ...] [LIBS ...] [DEFINES ...] [PTHREAD]
#            [ARGS ...] [SCENARIOS ...] [FIXTURES ...])
# Each scenario becomes its own test, <name>.<scenario>.
function(amity_test name)
    cmake_parse_arguments(PARSE_ARGV 1 A "PTHREAD" "" "SOURCES;INCLUDES;LIBS;DEFINES;ARGS;SCENARIOS;FIXTURES")
    add_executable(${name} ${A_SOURCES})
    # tests/fakes holds the fake headers shared by several tests; each test's
    # own fake directories come first.
    target_include_directories(${name} PRIVATE ${A_INCLUDES} "${T}/fakes")
    target_compile_definitions(${name} PRIVATE ${A_DEFINES})
    target_link_libraries(${name} PRIVATE ${A_LIBS} strict)
    if(A_PTHREAD)
        target_compile_options(${name} PRIVATE -pthread)
        target_link_options(${name} PRIVATE -pthread)
    endif()
    set(tests)
    if(A_SCENARIOS)
        foreach(scenario IN LISTS A_SCENARIOS)
            add_test(NAME ${name}.${scenario} COMMAND ${name} ${scenario} WORKING_DIRECTORY "${ROOT}")
            list(APPEND tests ${name}.${scenario})
        endforeach()
    else()
        add_test(NAME ${name} COMMAND ${name} ${A_ARGS} WORKING_DIRECTORY "${ROOT}")
        set(tests ${name})
    endif()
    set_tests_properties(${tests} PROPERTIES LABELS ${AMITY_TEST_LABEL} TIMEOUT 600)
    if(A_FIXTURES)
        set_tests_properties(${tests} PROPERTIES FIXTURES_REQUIRED "${A_FIXTURES}")
    endif()
endfunction()

function(amity_command_test name)
    add_test(NAME ${name} COMMAND ${ARGN} WORKING_DIRECTORY "${ROOT}")
    set_tests_properties(${name} PROPERTIES LABELS ${AMITY_TEST_LABEL} TIMEOUT 600)
endfunction()

# Script tests are named after their file.
function(amity_python_tests)
    foreach(script IN LISTS ARGN)
        amity_command_test(${script} "${PYTHON3}" "${T}/${script}")
    endforeach()
endfunction()

function(amity_node_tests)
    foreach(script IN LISTS ARGN)
        amity_command_test(${script} "${NODE}" "${T}/${script}")
    endforeach()
endfunction()

# Tests that need no ESP-IDF checkout, managed components or firmware build.
if(AMITY_HOST_TESTS MATCHES "^(all|standalone)$")
    set(AMITY_TEST_LABEL standalone)
    find_program(NODE node REQUIRED)
    find_program(GO go REQUIRED)

    # HomeKit SDK fork
    amity_test(homekit_input_test
        SOURCES "${T}/homekit_input_test.cpp"
            "${HAP}/esp_hap_core/src/esp_hap_pair_common.c" "${HAP}/esp_hap_core/src/esp_hap_pairings.c"
        INCLUDES "${T}/homekit_input_fakes" "${HAP}/esp_hap_core/src/priv_includes")
    set_target_properties(homekit_input_test PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    set_source_files_properties(
        "${HAP}/esp_hap_core/src/esp_hap_pair_common.c" "${HAP}/esp_hap_core/src/esp_hap_pairings.c"
        PROPERTIES COMPILE_OPTIONS -Wno-unused-parameter)
    amity_test(hap_http_test
        SOURCES "${HAP}/esp_hap_core/src/amity_hap_http.c" "${T}/hap_http_test.c"
        INCLUDES "${T}/hap_http_fakes" "${HAP}/esp_hap_core/src/priv_includes")
    set_target_properties(hap_http_test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    add_library(homekit_platform_httpd OBJECT "${HAP}/esp_hap_platform/src/hap_platform_httpd.c")
    target_include_directories(homekit_platform_httpd PRIVATE "${T}/homekit_platform_fakes")
    target_compile_options(homekit_platform_httpd PRIVATE -Wno-unused-function)
    target_link_libraries(homekit_platform_httpd PRIVATE strict)
    set_target_properties(homekit_platform_httpd PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    foreach(encryption plain encrypted)
        set(defines)
        if(encryption STREQUAL encrypted)
            set(defines CONFIG_NVS_ENCRYPTION=1)
        endif()
        amity_test(homekit_platform_test_${encryption} PTHREAD
            SOURCES "${HAP}/esp_hap_platform/src/hap_platform_keystore.cpp" "${T}/homekit_platform_test.cpp"
                $<TARGET_OBJECTS:homekit_platform_httpd>
            INCLUDES "${T}/homekit_platform_fakes" "${HAP}/esp_hap_platform/include"
            DEFINES ${defines})
    endforeach()
    amity_python_tests(homekit_http_test.py homekit_refresh_test.py homekit_worker_test.py)

    # Web server
    amity_test(web_request_test PTHREAD
        SOURCES "${C}/web/request.cpp" "${T}/web_request_test.cpp"
        INCLUDES "${T}/web_request_fakes" "${C}/web" "${C}/web/include" "${C}/system_services/include"
            "${C}/consortium/include" "${C}/amity_system/include")
    amity_python_tests(web_session_tcp_test.py web_redirect_test.py hostname_services_test.py
        original_controller_web_test.py)
    amity_node_tests(hostname_reconnect_ui_test.js poll_recovery_ui_test.js
        activity_choice_identity_ui_test.js membership_admission_ui_test.js membership_removal_ui_test.js
        log_download_ui_test.js firmware_version_test.js)
    amity_command_test(app_js_syntax "${NODE}" --check "${C}/web_admin/assets/app.js")
    amity_command_test(system_js_syntax "${NODE}" --check "${C}/web_admin/assets/system.js")

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
    amity_python_tests(hardware_capacity_test.py board_profile_config_test.py partition_staging_test.py
        removed_metadata_test.py)

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
            audio-key-held-deferred)
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
    amity_python_tests(key_release_timing_test.py)

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
            pairing-cleanup-retry absence success unpair unpair-delete-error unpair-storage-error
            unpair-before-completion unpair-pairing unpair-cancel-error connect-error async-error
            discovery-error disconnect-before-setup unreported-link-lost reconnect-backoff host-reset pairing
            stop-pairing-search stop-pairing-connect stop-pairing-security saved-layout disconnect-held-key
            changed-layout lost-layout-validation layout-without-hash)
    amity_test(ble_transport_no_privacy_test
        SOURCES ${ble_transport_sources}
        INCLUDES "${T}/ble_transport_fakes" "${C}/ble_remote/include"
        DEFINES MYNEWT_VAL_BLE_HS_PVCY=0
        SCENARIOS unpair-no-privacy)

    # Matter
    amity_test(diagnostics_test SOURCES "${T}/diagnostics_test.cpp" INCLUDES "${C}/amity_matter/include")
    amity_test(apple_fabric_cleanup_test SOURCES "${T}/apple_fabric_cleanup_test.cpp"
        INCLUDES "${C}/amity_matter/include")
    amity_python_tests(allocation_failure_diagnostics_test.py matter_state_publication_test.py
        matter_input_names_test.py controller_startup_test.py matter_auto_commissioning_test.py radio_matter_handoff_test.py
        apple_fabric_lifecycle_test.py controller_integration_test.py)

    # Amity consortium
    amity_test(consortium_test
        SOURCES "${T}/consortium_test.cpp"
        INCLUDES "${C}/consortium/include" "${C}/amity_system/include" "${C}/device_settings/include"
        LIBS amity_model amity_consortium
        ARGS "${ROOT}")
    amity_test(consortium_logs_codec_test
        SOURCES "${T}/consortium_logs_codec_test.cpp"
        INCLUDES "${C}/consortium/include" "${C}/amity_system/include" "${C}/device_settings/include"
        LIBS amity_model amity_consortium)
    amity_python_tests(admission_workflow_test.py consortium_admission_crypto_test.py
        consortium_admission_startup_test.py consortium_gatt_callback_test.py consortium_gatt_fragment_test.py
        consortium_gatt_admission_test.py consortium_runtime_admission_test.py consortium_live_configuration_test.py
        consortium_logs_runtime_test.py log_download_test.py consortium_synchronization_recovery_test.py
        consortium_firmware_identity_test.py consortium_enrollment_restart_test.py consortium_tls_rejection_test.py
        consortium_setup_timing_test.py consortium_tls_receive_test.py consortium_worker_poll_test.py
        consortium_task_lifetime_test.py consortium_runtime_status_test.py runtime_peer_status_test.py
        consortium_original_claim_test.py consortium_group_storage_test.py consortium_owner_repeat_test.py
        consortium_tcp_ack_trace_test.py wifi_signal_test.py console_latency_observer_test.py
        consortium_hostname_test.py consortium_network_connect_test.py consortium_settings_test.py
        consortium_removal_test.py consortium_persistence_test.py wifi_latency_policy_test.py)

    # Firmware updates and release tooling
    amity_test(firmware_version_test
        SOURCES "${T}/firmware_version_test.cpp"
        INCLUDES "${C}/system_services/include"
        ARGS "${T}/firmware_versions.tsv")
    amity_command_test(release_manifest_fixture "${PYTHON3}" "${T}/release_manifest_fixture.py"
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
    amity_python_tests(release_hardware_test.py update_bundle_test.py production_profile_test.py
        release_scripts_test.py)

    # Setup tool
    amity_command_test(provisioning_go_test "${GO}" test ./...)
    amity_command_test(provisioning_go_vet "${GO}" vet ./...)
    set_tests_properties(provisioning_go_test provisioning_go_vet PROPERTIES
        WORKING_DIRECTORY "${ROOT}/amity-provision"
        ENVIRONMENT "GOCACHE=${ROOT}/.cache/go-build;GOMODCACHE=${ROOT}/.cache/go-mod")
endif()

# Tests that read ESP-IDF sources, managed components or firmware build output.
if(AMITY_HOST_TESTS MATCHES "^(all|sdk)$")
    set(AMITY_TEST_LABEL sdk)
    set(json "${ROOT}/managed_components/espressif__cjson/cJSON")
    if(NOT EXISTS "${json}/cJSON.c")
        set(json "${ROOT}/managed_components/espressif__cjson")
    endif()
    if(NOT EXISTS "${json}/cJSON.c")
        message(FATAL_ERROR "managed_components/espressif__cjson is missing. Build the firmware once, "
            "or configure with -DAMITY_HOST_TESTS=standalone.")
    endif()
    add_library(cjson STATIC "${json}/cJSON.c")
    target_include_directories(cjson PRIVATE "${json}")
    target_compile_options(cjson PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(cjson PRIVATE sanitized)

    amity_test(json_input_test
        SOURCES "${T}/json_input_test.cpp"
        INCLUDES "${json}" "${C}/web_admin/include"
        LIBS cjson)
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
    amity_test(local_remote_approval_test
        SOURCES "${T}/local_remote_approval_test.cpp"
        INCLUDES "${json}" "${C}/web_admin" "${C}/web_admin/include" "${C}/amity_system/include"
        LIBS amity_model cjson)

    amity_python_tests(homekit_notifications_test.py https_response_write_test.py esp_aes_dma_cleanup_test.py
        mdns_delegate_remove_test.py protocol_memory_policy_test.py web_stall_test.py admin_rejection_test.py
        hostname_reconnect_api_test.py membership_admission_api_test.py membership_capacity_api_test.py
        membership_removal_api_test.py controller_api_surface_test.py system_peer_record_test.py
        network_diagnostics_test.py lwip_tcp_timer_test.py configuration_snapshot_web_test.py json_memory_test.py
        consortium_tls_tcp_test.py consortium_tls_credentials_test.py web_tls_identity_test.py
        homekit_crypto_test.py)
    # These configure and build one shared host Mbed TLS tree.
    set_tests_properties(consortium_tls_tcp_test.py consortium_tls_credentials_test.py web_tls_identity_test.py
        homekit_crypto_test.py PROPERTIES RESOURCE_LOCK host_mbedtls)
endif()
