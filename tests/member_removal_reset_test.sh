#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "$root/test-results/member-removal.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/tests/member_removal_fakes" -I"$root/tests/reset_fakes" -I"$root/tests/cohort_fakes" \
 -I"$root/components/system_services/include" -I"$root/components/device_settings/include" \
 -I"$root/components/config_store/include" -I"$root/components/amity_system/include" \
 "$root/components/system_services/member_removal.cpp" "$root/components/device_settings/factory_reset.cpp" \
 "$root/components/amity_system/model.cpp" "$root/tests/member_removal_reset_test.cpp" -o "$out/test"
"$out/test"
