#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results/host"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/components/amity_system/include" "$root/components/amity_system/model.cpp" "$root/tests/addressing_test.cpp" -o "$root/test-results/host/addressing_test"
"$root/test-results/host/addressing_test"
