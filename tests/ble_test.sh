#!/bin/sh
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
component_dir="$project_dir/components/ble_remote"
test_dir="$project_dir/tests"
result_dir="$project_dir/test-results"
mkdir -p "$result_dir"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/ble_keyboard_test.cpp" \
    "$component_dir/hid_keyboard.cpp" "$component_dir/atomic_remote.cpp" "$component_dir/remote_input.cpp" \
    -o "$result_dir/ble_keyboard_test"
"$result_dir/ble_keyboard_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/ble_controller_test.cpp" \
    "$component_dir/atomic_remote.cpp" "$component_dir/remote_input.cpp" \
    -o "$result_dir/ble_controller_test"
"$result_dir/ble_controller_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/siri_remote_test.cpp" \
    "$component_dir/siri_remote.cpp" "$component_dir/remote_input.cpp" -o "$result_dir/siri_remote_test"
"$result_dir/siri_remote_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/remote_input_test.cpp" \
    "$component_dir/atomic_remote.cpp" "$component_dir/siri_remote.cpp" \
    "$component_dir/remote_input.cpp" -o "$result_dir/remote_input_test"
"$result_dir/remote_input_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$project_dir/main" "$test_dir/key_sequence_test.cpp" -o "$result_dir/key_sequence_test"
"$result_dir/key_sequence_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/gatt_layout_test.cpp" \
    "$component_dir/gatt_layout.cpp" -o "$result_dir/gatt_layout_test"
"$result_dir/gatt_layout_test"
sh "$test_dir/ble_transport_test.sh"
