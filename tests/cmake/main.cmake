# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Firmware startup and the main task: main/main.cpp.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_python_tests(
        controller_startup_test.py
        key_release_timing_test.py
        matter_auto_commissioning_test.py
        wifi_latency_policy_test.py
    )
endif()
