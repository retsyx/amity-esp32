#!/bin/sh
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
python3 "$root/tests/hardware_capacity_test.py"
sh "$root/tests/homekit_input_test.sh"
sh "$root/tests/hap_http_test.sh"
python3 "$root/tests/homekit_http_test.py"
sh "$root/tests/web_request_test.sh"
python3 "$root/tests/https_response_write_test.py"
python3 "$root/tests/admin_rejection_test.py"
python3 "$root/tests/hostname_reconnect_api_test.py"
python3 "$root/tests/hostname_services_test.py"
node "$root/tests/hostname_reconnect_ui_test.js"
node "$root/tests/activity_choice_identity_ui_test.js"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/config_store_fakes" -I"$root/tests/reset_fakes" -I"$root/components/config_store/include" \
  "$root/components/config_store/config_store.cpp" "$root/tests/config_store_test.cpp" -o "$out/config_store_test"
"$out/config_store_test"
sh "$root/tests/cec_hub_test.sh"
sh "$root/tests/owned_hub_test.sh"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/reset_fakes" -I"$root/components/amity_system/include" -I"$root/components/device_settings/include" \
  "$root/components/device_settings/reset_button.cpp" "$root/tests/reset_button_test.cpp" -o "$out/reset_button_test"
"$out/reset_button_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/reset_button_fakes" -I"$root/tests/reset_fakes" \
  -I"$root/components/amity_system/include" -I"$root/components/device_settings/include" "$root/components/device_settings/reset_button.cpp" \
  "$root/tests/reset_button_interrupt_test.cpp" -o "$out/reset_button_interrupt_test"
for scenario in idle boot-held hold short cancel boot-release-hold latched-edge retry \
  allocation-failure shared-service-failure handler-failure setup-short setup-before-three-seconds \
  setup-three-seconds setup-release-bounce setup-press-bounce-short setup-press-bounce-three-seconds \
  setup-before-reset setup-reset-release setup-after-reset setup-long setup-boot-held; do
  "$out/reset_button_interrupt_test" "$scenario"
done
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/components/ble_remote/hid_keyboard.cpp" \
  -x c++ "$root/components/ble_remote/atomic_remote.c" "$root/tests/ble_keyboard_test.cpp" -o "$out/ble_test"
"$out/ble_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/components/ble_remote/atomic_controller.cpp" "$root/components/ble_remote/remote_input.cpp" \
  -x c++ "$root/components/ble_remote/atomic_remote.c" "$root/tests/ble_controller_test.cpp" -o "$out/ble_controller_test"
"$out/ble_controller_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/components/ble_remote/siri_remote.cpp" \
  "$root/components/ble_remote/remote_input.cpp" "$root/tests/siri_remote_test.cpp" -o "$out/siri_remote_test"
"$out/siri_remote_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/tests/remote_input_test.cpp" \
  "$root/components/ble_remote/atomic_controller.cpp" "$root/components/ble_remote/siri_remote.cpp" \
  "$root/components/ble_remote/remote_input.cpp" -x c++ "$root/components/ble_remote/atomic_remote.c" \
  -o "$out/remote_input_test"
"$out/remote_input_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/main" "$root/tests/swipe_sequence_test.cpp" -o "$out/swipe_sequence_test"
"$out/swipe_sequence_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/tests/pairing_policy_test.cpp" -o "$out/pairing_policy_test"
"$out/pairing_policy_test"
sh "$root/tests/ble_transport_test.sh"
json="$root/managed_components/espressif__cjson/cJSON"
test -f "$json/cJSON.c" || json="$root/managed_components/espressif__cjson"
cc -g -Wno-deprecated-declarations -fsanitize=address,undefined -I"$json" -c "$json/cJSON.c" -o "$out/cjson.o"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$json" -I"$root/components/web_admin/include" \
  "$root/tests/json_input_test.cpp" "$out/cjson.o" -o "$out/json_input_test"
"$out/json_input_test"
node --check "$root/components/web_admin/assets/app.js"
node --check "$root/components/web_admin/assets/system.js"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/system_services/include" "$root/tests/firmware_version_test.cpp" -o "$out/firmware_version_test"
"$out/firmware_version_test" "$root/tests/firmware_versions.tsv"
node "$root/tests/firmware_version_test.js"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/amity_system/include" -I"$root/components/device_settings/include" "$root/tests/physical_address_test.cpp" -o "$out/physical_address_test"
"$out/physical_address_test"
sh "$root/tests/device_settings_test.sh"
sh "$root/tests/adapter_settings_test.sh"
sh "$root/tests/member_removal_reset_test.sh"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/tests/battery_status_test.cpp" -o "$out/battery_status_test"
"$out/battery_status_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/amity_matter/include" "$root/tests/diagnostics_test.cpp" -o "$out/diagnostics_test"
"$out/diagnostics_test"
python3 "$root/tests/allocation_failure_diagnostics_test.py"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/amity_matter/include" "$root/tests/apple_fabric_cleanup_test.cpp" -o "$out/apple_fabric_cleanup_test"
"$out/apple_fabric_cleanup_test"
sh "$root/tests/identity_journal_test.sh"
sh "$root/tests/addressing_test.sh"
sh "$root/tests/cec_runtime_test.sh"
sh "$root/tests/consortium_test.sh"
python3 "$root/tests/admission_workflow_test.py"
python3 "$root/tests/consortium_admission_crypto_test.py"
python3 "$root/tests/consortium_admission_startup_test.py"
python3 "$root/tests/membership_admission_api_test.py"
python3 "$root/tests/membership_removal_api_test.py"
node "$root/tests/membership_admission_ui_test.js"
node "$root/tests/membership_removal_ui_test.js"
python3 "$root/tests/consortium_gatt_callback_test.py"
python3 "$root/tests/consortium_gatt_fragment_test.py"
python3 "$root/tests/consortium_gatt_admission_test.py"
python3 "$root/tests/consortium_runtime_admission_test.py"
python3 "$root/tests/consortium_live_configuration_test.py"
python3 "$root/tests/consortium_synchronization_recovery_test.py"
python3 "$root/tests/consortium_firmware_identity_test.py"
python3 "$root/tests/consortium_enrollment_restart_test.py"
python3 "$root/tests/consortium_tls_rejection_test.py"
python3 "$root/tests/consortium_tls_credentials_test.py"
python3 "$root/tests/consortium_tls_tcp_test.py"
python3 "$root/tests/consortium_setup_timing_test.py"
python3 "$root/tests/consortium_tls_receive_test.py"
python3 "$root/tests/consortium_worker_poll_test.py"
python3 "$root/tests/consortium_task_lifetime_test.py"
python3 "$root/tests/consortium_runtime_status_test.py"
python3 "$root/tests/runtime_peer_status_test.py"
python3 "$root/tests/original_controller_web_test.py"
python3 "$root/tests/controller_api_surface_test.py"
python3 "$root/tests/consortium_original_claim_test.py"
python3 "$root/tests/consortium_retired_features_test.py"
python3 "$root/tests/consortium_owner_repeat_test.py"
python3 "$root/tests/consortium_hostname_test.py"
python3 "$root/tests/removed_metadata_test.py"
python3 "$root/tests/consortium_network_connect_test.py"
python3 "$root/tests/consortium_settings_test.py"
python3 "$root/tests/consortium_removal_test.py"
python3 "$root/tests/controller_integration_test.py"
python3 "$root/tests/controller_startup_test.py"
python3 "$root/tests/matter_auto_commissioning_test.py"
python3 "$root/tests/radio_matter_handoff_test.py"
python3 "$root/tests/apple_fabric_lifecycle_test.py"

sh "$root/tests/system_configuration_test.sh"
python3 "$root/tests/json_memory_test.py"
sh "$root/tests/activity_editor_test.sh"
sh "$root/tests/native_ports_test.sh"
sh "$root/tests/system_sync_test.sh"
sh "$root/tests/release_manifest_test.sh"
python3 "$root/tests/release_hardware_test.py"
python3 "$root/tests/update_bundle_test.py"
python3 "$root/tests/partition_staging_test.py"
sh "$root/tests/system_remote_control_test.sh"
sh "$root/tests/local_remote_approval_test.sh"
sh "$root/tests/cohort_update_test.sh"

sh "$root/tests/configuration_replacement_test.sh"
PYTHONPATH="$root/cec-harness" python3 -m unittest discover -s "$root/cec-harness/unit_tests" -q

python3 "$root/tests/production_profile_test.py"
