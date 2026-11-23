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

    amity_python_tests(
        consortium_network_connect_test.py
        radio_matter_handoff_test.py
    )
endif()
