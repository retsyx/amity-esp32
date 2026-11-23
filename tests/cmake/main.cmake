# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Firmware startup and the main task: main/main.cpp.
if(AMITY_TEST_TIER STREQUAL standalone)
    set(main_includes "${T}/core_fakes" "${T}/main_fakes" "${T}/web_request_fakes" "${T}" "${ROOT}/main"
        "${C}/amity_hub/include" "${C}/amity_matter/include" "${C}/amity_homekit/include" "${C}/ble_remote/include"
        "${C}/config_store/include" "${C}/consortium/include" "${C}/amity_system/include" "${C}/system_sync/include"
        "${C}/radio_control/include" "${C}/device_settings/include" "${C}/system_services/include"
        "${C}/network_diagnostics/include" "${C}/web_admin/include" "${C}/cec/include")
    # Strict seams for the SDK calls of tests/main_fakes and for the firmware
    # files main.cpp calls that tests/component_seams does not cover.
    set(main_seams)
    foreach(file sdk web_admin system_sync config_store json_memory system_configuration)
        add_library(main_${file}_seams OBJECT "${T}/main_fakes/${file}_seams.cpp")
        target_include_directories(main_${file}_seams PRIVATE ${main_includes} "${T}/fakes")
        target_compile_options(main_${file}_seams PRIVATE ${amity_seam_warnings})
        target_link_libraries(main_${file}_seams PRIVATE strict)
        list(APPEND main_seams main_${file}_seams)
    endforeach()
    # main.cpp builds on its own, and inside main_test, which includes it. The
    # firmware files it reaches that build on the host are linked: the hub
    # facade, the configuration journal and the remote input decoders.
    amity_host_compile(main_source SOURCES "${ROOT}/main/main.cpp" INCLUDES ${main_includes})
    amity_host_compile(main_host_sources
        SOURCES "${C}/amity_hub/system_control.cpp" "${C}/amity_hub/remote_control.cpp"
            "${C}/config_store/configuration_journal.cpp" "${C}/ble_remote/remote_input.cpp"
            "${C}/ble_remote/atomic_remote.cpp" "${C}/ble_remote/siri_remote.cpp"
        INCLUDES ${main_includes})
    set(main_libs main_host_sources ${main_seams} consortium_runtime_seams system_services_seams
        adapter_settings_seams member_removal_seams firmware_update_seams group_firmware_update_seams
        peer_firmware_update_seams device_settings_seams factory_reset_seams reset_button_seams
        network_diagnostics_seams wifi_signal_seams system_identity_seams hub_service_seams amity_matter_seams
        homekit_seams ble_remote_seams radio_seams core_fakes amity_model)
    set(main_scenarios
        boot-unclaimed boot-member boot-original boot-original-unnamed-responder boot-original-without-consortium
        boot-original-unreadable boot-original-without-authority
        retire-headless retire-published retire-fence-fails retire-web-fails retire-matter-fails retire-homekit-fails
        retire-errors-ranked retire-later-errors-ranked retire-mdns-removal-fails retire-unregistered retire-in-flight
        setup-button-matter setup-button-add
        await-claim await-member await-claim-lost await-prepared await-recovery
        matter-boot-unclaimed matter-boot-configured matter-boot-original matter-boot-fabric matter-boot-original-fabrics
        matter-boot-member matter-boot-prepared matter-boot-retired matter-boot-recovery matter-boot-uninitialized
        matter-boot-without-authority matter-boot-enrollment-open matter-boot-firmware-not-ready
        matter-boot-without-consortium matter-boot-without-activation matter-boot-status-invalid matter-boot-open-retry
        matter-boot-status-retry matter-boot-open-exhausted matter-boot-status-exhausted matter-boot-open-invalid
        matter-boot-fabric-appears matter-boot-role-changes matter-boot-authority-lost matter-button-reopens
        wifi-policy-unclaimed wifi-policy-member wifi-policy-already-off wifi-policy-max-modem wifi-policy-matter-fails
        wifi-policy-read-fails wifi-policy-set-fails wifi-policy-verify-fails wifi-policy-stays-on
        release-timing
        rename-published rename-back rename-alias-fails rename-address-fails rename-https-fails rename-homekit-fails
        rename-old-alias-fails rename-network-fails rename-without-station rename-https-busy rename-https-exhausted
        rename-after-failed-registration rename-member)
    # app_main has two #if blocks. main_test builds with both settings off and
    # main_production_test with both on, so a step inside an #if block fails
    # one of them.
    amity_test(main_test
        SOURCES "${T}/main_test.cpp"
        INCLUDES ${main_includes}
        LIBS ${main_libs}
        PTHREAD
        REGISTRY
        SCENARIOS ${main_scenarios})
    amity_test(main_production_test
        SOURCES "${T}/main_test.cpp"
        INCLUDES ${main_includes}
        DEFINES CONFIG_SPIRAM_USE_MALLOC=1 CONFIG_AMITY_PRODUCTION_UPDATES=1
        LIBS ${main_libs}
        PTHREAD
        REGISTRY
        SCENARIOS ${main_scenarios})
    # As in the firmware build, an unused function is only a warning, so a
    # step that one build compiles out fails that build's scenarios.
    target_compile_options(main_test PRIVATE -Wno-error=unused-function)
    target_compile_options(main_production_test PRIVATE -Wno-error=unused-function)
    # The hub's side of a remote key release: its dequeue, owner-send and
    # fallback timing logs, captured by including hub_service.cpp.
    amity_test(key_release_timing_test
        SOURCES "${C}/amity_hub/hub.cpp" "${T}/key_release_timing_test.cpp"
        INCLUDES "${T}" "${T}/cec_hub_fakes" "${C}/amity_system/include" "${C}/config_store/include"
            "${C}/cec/include" "${C}/amity_hub/include" "${C}/amity_hub"
        LIBS amity_model
        REGISTRY
        SCENARIOS queue-owner-handoff owner-repeats-unlogged)
endif()
