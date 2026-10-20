#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host";mkdir -p "$out"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/components/consortium/include" -I"$root/components/amity_system/include" \
 "$root/components/consortium/bootstrap_record.cpp" "$root/tests/bootstrap_rotation_test.cpp" -o "$out/bootstrap_rotation_test"
"$out/bootstrap_rotation_test"
