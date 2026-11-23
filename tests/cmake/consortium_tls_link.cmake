# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# The consortium TLS link, built from tls_link.cpp over scripted Mbed TLS
# sessions and sockets. Never link it with a scripted TlsLink.
if(AMITY_TEST_TIER STREQUAL standalone)
    set(tls_link_includes "${T}/tls_link_fakes" "${T}/core_fakes" "${C}/consortium" "${C}/consortium/include"
        "${C}/amity_system/include")
    add_library(tls_link_fakes OBJECT "${T}/tls_link_fakes/tls_link_fakes.cpp")
    target_include_directories(tls_link_fakes PRIVATE ${tls_link_includes} "${T}")
    target_compile_options(tls_link_fakes PRIVATE ${amity_seam_warnings})
    target_link_libraries(tls_link_fakes PRIVATE strict)
    amity_host_compile(consortium_tls_link
        SOURCES "${C}/consortium/tls_link.cpp"
        INCLUDES ${tls_link_includes})
    amity_test(consortium_tls_link_test
        SOURCES "${T}/consortium_tls_link_test.cpp"
        INCLUDES ${tls_link_includes}
        LIBS consortium_tls_link tls_link_fakes consortium_setup_timing core_fakes amity_consortium amity_model
        REGISTRY
        SCENARIOS lazy-handshake-and-transport-health ipc-during-partial-frame fragments-and-frame-boundaries
            terminal-deadlines-and-errors blocking-wrappers socket-callbacks bounded-application-writes
            bulk-transfers terminal-receive-diagnostics expired-frames-drain missing-client-certificate
            queued-alert-flushed blocked-alert rejection-drain-ends client-does-not-reject unsettable-socket
            valid-handshake-once unauthenticated-handshake bad-verify-flags verify-callback setup-configuration
            setup-refusals handshake-deadline admission-slots teardown)
    unset(tls_link_includes)
endif()
