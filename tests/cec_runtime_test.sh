#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results/host"
binary="$root/test-results/host/cec_runtime_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/components/cec/include" "$root/components/cec/engine.cpp" \
    "$root/components/cec/scheduler.cpp" "$root/tests/cec_runtime_test.cpp" -o "$binary"
"$binary"
python3 "$root/tests/board_profile_config_test.py"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/tests/cec_runtime_fakes" -I"$root/components/cec/include" \
    "$root/components/cec/engine.cpp" "$root/components/cec/scheduler.cpp" \
    "$root/components/cec/bus.cpp" "$root/tests/cec_runtime_driver_test.cpp" -o "$binary"
"$binary"
