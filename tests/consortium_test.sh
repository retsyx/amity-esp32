#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/consortium/include" -I"$root/components/amity_system/include" \
  -I"$root/components/device_settings/include" \
  "$root"/components/consortium/*.cpp "$root/components/amity_system/model.cpp" \
  "$root/tests/consortium_test.cpp" -o "$out/consortium_test"
"$out/consortium_test" "$root"
