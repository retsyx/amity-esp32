# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Pinned ESP-IDF and managed-component code, run against Amity's configuration and patches.
if(AMITY_TEST_TIER STREQUAL sdk)
    amity_python_tests(
        esp_aes_dma_cleanup_test.py
        lwip_tcp_timer_test.py
        mdns_delegate_remove_test.py
        network_diagnostics_test.py
        protocol_memory_policy_test.py
    )
endif()
