#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
component_dir="$project_dir/components/ble_remote"
test_dir="$project_dir/tests"
result_dir="$project_dir/test-results"
mkdir -p "$result_dir"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/ble_keyboard_test.cpp" \
    "$component_dir/hid_keyboard.cpp" -x c++ "$component_dir/atomic_remote.c" \
    -o "$result_dir/ble_keyboard_test"
"$result_dir/ble_keyboard_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/ble_controller_test.cpp" \
    "$component_dir/atomic_controller.cpp" -x c++ "$component_dir/atomic_remote.c" \
    -o "$result_dir/ble_controller_test"
"$result_dir/ble_controller_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" "$test_dir/siri_remote_test.cpp" \
    "$component_dir/siri_remote.cpp" -o "$result_dir/siri_remote_test"
"$result_dir/siri_remote_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$project_dir/main" "$test_dir/swipe_sequence_test.cpp" -o "$result_dir/swipe_sequence_test"
"$result_dir/swipe_sequence_test"
sh "$test_dir/ble_transport_test.sh"
