# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Consortium GATT transport and the admission exchange it carries.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_python_tests(
        consortium_admission_crypto_test.py
        consortium_gatt_admission_test.py
        consortium_gatt_callback_test.py
        consortium_gatt_fragment_test.py
    )
endif()
