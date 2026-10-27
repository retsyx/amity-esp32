#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "$root/test-results/device-settings.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/tests/device_settings_fakes" -I"$root/tests/reset_fakes" -I"$root/tests/cec_hub_fakes" \
 -I"$root/components/amity_system/include" -I"$root/components/device_settings/include" -I"$root/components/config_store/include" \
 "$root/components/device_settings/device_settings.cpp" "$root/tests/device_settings_test.cpp" -o "$out/test"
"$out/test"
