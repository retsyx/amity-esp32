# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Consortium GATT transport and the admission exchange it carries.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_host_compile(consortium_admission_crypto
        SOURCES "${C}/consortium/admission_crypto.cpp"
        INCLUDES "${T}/crypto_fakes" "${C}/consortium")
    amity_test(consortium_admission_crypto_test
        SOURCES "${T}/consortium_admission_crypto_test.cpp"
        INCLUDES "${T}/crypto_fakes" "${C}/consortium"
        LIBS consortium_admission_crypto crypto_fakes
        REGISTRY
        SCENARIOS round-trip replay key-generation-failure reflected-direction skipped-sequence tampered-ciphertext
            tampered-tag malformed-header cross-session edited-transcript tampered-confirmation invalid-server-id
            one-handshake record-size-limit repeated-finish)
    amity_python_tests(
        consortium_gatt_admission_test.py
        consortium_gatt_callback_test.py
        consortium_gatt_fragment_test.py
    )
endif()
