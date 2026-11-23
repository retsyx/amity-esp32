# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# System administration API.
if(AMITY_TEST_TIER STREQUAL sdk)
    amity_python_tests(
        admin_rejection_test.py
        configuration_snapshot_web_test.py
        membership_admission_api_test.py
        membership_removal_api_test.py
    )
endif()
