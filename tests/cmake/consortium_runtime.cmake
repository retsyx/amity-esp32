# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Consortium runtime: membership, links, synchronization and the consortium codecs.
if(AMITY_TEST_TIER STREQUAL standalone)
    amity_test(consortium_test
        SOURCES "${T}/consortium_test.cpp"
        INCLUDES "${C}/consortium/include" "${C}/amity_system/include" "${C}/device_settings/include"
        LIBS amity_model amity_consortium
        ARGS "${ROOT}")
    amity_test(consortium_logs_codec_test
        SOURCES "${T}/consortium_logs_codec_test.cpp"
        INCLUDES "${C}/consortium/include" "${C}/amity_system/include" "${C}/device_settings/include"
        LIBS amity_model amity_consortium)
    amity_python_tests(
        admission_workflow_test.py
        consortium_admission_startup_test.py
        consortium_enrollment_restart_test.py
        consortium_firmware_identity_test.py
        consortium_hostname_test.py
        consortium_live_configuration_test.py
        consortium_logs_runtime_test.py
        consortium_original_claim_test.py
        consortium_owner_repeat_test.py
        consortium_persistence_test.py
        consortium_removal_test.py
        consortium_runtime_admission_test.py
        consortium_runtime_status_test.py
        consortium_settings_test.py
        consortium_synchronization_recovery_test.py
        consortium_task_lifetime_test.py
        consortium_tcp_ack_trace_test.py
        consortium_worker_poll_test.py
        removed_metadata_test.py
        runtime_peer_status_test.py
    )
endif()
