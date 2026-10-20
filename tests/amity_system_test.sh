#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results"
binary=$(mktemp "$root/test-results/amity-system.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/components/amity_system/include" \
    "$root/components/amity_system/model.cpp" "$root/components/amity_system/discovery.cpp" \
    "$root/components/amity_system/policy.cpp" "$root/components/amity_system/planner.cpp" \
    "$root/tests/amity_system_test.cpp" -o "$binary"
"$binary"
