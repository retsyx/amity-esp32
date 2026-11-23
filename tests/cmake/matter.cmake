# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Matter, the radio handoff and the consortium network hooks.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_test(diagnostics_test SOURCES "${T}/diagnostics_test.cpp" INCLUDES "${C}/amity_matter/include")
    amity_test(apple_fabric_cleanup_test SOURCES "${T}/apple_fabric_cleanup_test.cpp"
        INCLUDES "${C}/amity_matter/include")

    # The CHIP fakes: the CHIP task and its stack lock, System Layer timers, and
    # strict seams for the SDK calls that answer questions. Tests list
    # matter_includes first among their INCLUDES.
    add_library(amity_matter_fakes OBJECT "${T}/amity_matter_fakes/amity_matter_fakes.cpp")
    target_include_directories(amity_matter_fakes PRIVATE "${T}/core_fakes" "${T}/amity_matter_fakes" "${T}")
    target_compile_options(amity_matter_fakes PRIVATE ${amity_seam_warnings})
    target_link_libraries(amity_matter_fakes PRIVATE strict)
    set(matter_includes "${T}/core_fakes" "${T}/amity_matter_fakes")

    amity_host_compile(apple_fabrics
        SOURCES "${C}/amity_matter/apple_fabrics.cpp"
        INCLUDES ${matter_includes} "${C}/amity_matter/include")
    amity_test(apple_fabrics_test
        SOURCES "${T}/apple_fabrics_test.cpp"
        INCLUDES ${matter_includes} "${C}/amity_matter/include"
        LIBS amity_matter_fakes core_fakes
        REGISTRY
        SCENARIOS stop-fences-queued-work fail-safe-retry cleanup public-functions storage-errors delegate-refused
            scheduling-retry deletion-retry last-home-removed home-committed commit-reschedules pending-fabric-ignored)

    # Clang warns about the CHIP_ERROR returned by the extern "C" link
    # wrappers; GCC, which builds the firmware, accepts them.
    set(radio_includes ${matter_includes} "${T}/radio_control_fakes" "${C}/radio_control/include"
        "${C}/consortium/include" "${C}/amity_system/include" "${C}/ble_remote/include")
    set_source_files_properties("${C}/radio_control/radio_control.cpp" "${T}/radio_matter_handoff_test.cpp"
        PROPERTIES COMPILE_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-return-type-c-linkage>)
    amity_host_compile(radio_control SOURCES "${C}/radio_control/radio_control.cpp" INCLUDES ${radio_includes})
    amity_test(radio_matter_handoff_test
        SOURCES "${T}/radio_matter_handoff_test.cpp"
        INCLUDES ${radio_includes}
        LIBS amity_matter_fakes core_fakes consortium_runtime_seams ble_remote_seams amity_consortium amity_model
        REGISTRY
        SCENARIOS idle-handoff-candidate idle-handoff-inviter active-denied other-busy stale-disable stale-cleanup
            bounded-timeout shared-budget peripheral-during-disable disable-refused handoff-after-other-owner
            late-cleanup-handoff late-cleanup-release late-cleanup-result late-advertising-cleanup ordinary
            worker-consumes-disable worker-finishes-cleanup disable-before-disconnect disconnect-before-disable
            unrelated-events ip-state-releases ip-state-admits committed-disable-retry disable-fences-generation
            late-peripheral-in-transition expired-workflow disable-before-matter-owns invalid-role table-overflow
            hci-connection-forms hci-minimum-lengths hci-before-delivery hci-under-radio-mutex controller-reset start
            start-failures remote-hooks advertising-refused)

    # As in the firmware build, the hooks' aggregate initializer may leave the
    # hostname hooks to the assignments that follow it.
    set(network_includes ${matter_includes} "${T}/crypto_fakes" "${C}/consortium/include" "${C}/amity_system/include"
        "${C}/device_settings/include")
    set_source_files_properties("${C}/amity_matter/consortium_network.cpp" "${T}/consortium_network_test.cpp"
        PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    amity_host_compile(consortium_network
        SOURCES "${C}/amity_matter/consortium_network.cpp"
        INCLUDES ${network_includes})
    amity_test(consortium_network_test PTHREAD
        SOURCES "${T}/consortium_network_test.cpp"
        INCLUDES ${network_includes}
        LIBS amity_matter_fakes core_fakes crypto_fakes consortium_runtime_seams device_settings_seams amity_consortium
            amity_model
        REGISTRY
        SCENARIOS address-at-6s address-at-34900ms immediate-address failed-association absent-callback address-never
            address-at-deadline address-after-deadline single-budget deadline-between-polls remove-failure add-failure
            stored-networks refused commit-and-rollback transaction-busy driver-calls exported-credentials
            hostname-hooks install-failure)
endif()
