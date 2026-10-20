#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp "$root/test-results/system-hold.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/components/amity_system/include" "$root/components/amity_system/model.cpp" \
 "$root/components/amity_system/discovery.cpp" "$root/components/amity_system/policy.cpp" \
 "$root/components/amity_system/planner.cpp" "$root/components/amity_system/coordinator.cpp" \
 "$root/tests/system_hold_test.cpp" -o "$binary"
"$binary"
