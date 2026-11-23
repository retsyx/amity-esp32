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
    amity_python_tests(
        apple_fabric_lifecycle_test.py
        consortium_network_connect_test.py
        radio_matter_handoff_test.py
    )
endif()
