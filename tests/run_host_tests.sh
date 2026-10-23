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
sh "$root/tests/homekit_input_test.sh"
sh "$root/tests/hap_http_test.sh"
python3 "$root/tests/homekit_http_test.py"
sh "$root/tests/web_request_test.sh"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/config_store_fakes" -I"$root/tests/reset_fakes" -I"$root/components/config_store/include" \
  "$root/components/config_store/config_store.cpp" "$root/tests/config_store_test.cpp" -o "$out/config_store_test"
"$out/config_store_test"
sh "$root/tests/cec_hub_test.sh"
sh "$root/tests/cec_bus_test.sh"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/reset_fakes" -I"$root/components/device_settings/include" \
  "$root/components/device_settings/reset_button.cpp" "$root/tests/reset_button_test.cpp" -o "$out/reset_button_test"
"$out/reset_button_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/reset_button_fakes" -I"$root/tests/reset_fakes" \
  -I"$root/components/device_settings/include" "$root/components/device_settings/reset_button.cpp" \
  "$root/tests/reset_button_interrupt_test.cpp" -o "$out/reset_button_interrupt_test"
for scenario in idle boot-held hold short cancel boot-release-hold latched-edge retry \
  allocation-failure shared-service-failure handler-failure; do
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
  -I"$json" -I"$root/components/web_admin/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
  "$root/components/web_admin/configuration.cpp" "$root/tests/configuration_test.cpp" "$out/cjson.o" -o "$out/configuration_test"
"$out/configuration_test"
node --check "$root/components/web_admin/assets/app.js"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/system_services/include" "$root/tests/firmware_version_test.cpp" -o "$out/firmware_version_test"
"$out/firmware_version_test" "$root/tests/firmware_versions.tsv"
node "$root/tests/firmware_version_test.js"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/device_settings/include" "$root/tests/physical_address_test.cpp" -o "$out/physical_address_test"
"$out/physical_address_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/ble_remote/include" "$root/tests/battery_status_test.cpp" -o "$out/battery_status_test"
"$out/battery_status_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/amity_matter/include" "$root/tests/diagnostics_test.cpp" -o "$out/diagnostics_test"
"$out/diagnostics_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/amity_matter/include" "$root/tests/apple_fabric_cleanup_test.cpp" -o "$out/apple_fabric_cleanup_test"
"$out/apple_fabric_cleanup_test"
