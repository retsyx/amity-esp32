#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "$root/test-results/local-remote.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
json="$root/managed_components/espressif__cjson/cJSON"
test -f "$json/cJSON.c" || json="$root/managed_components/espressif__cjson"
cc -g -Wno-deprecated-declarations -fsanitize=address,undefined -I"$json" -c "$json/cJSON.c" -o "$out/cjson.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$json" -I"$root/components/web_admin" -I"$root/components/web_admin/include" -I"$root/components/amity_system/include" \
 "$root/components/amity_system/model.cpp" "$root/tests/local_remote_approval_test.cpp" "$out/cjson.o" -o "$out/test"
"$out/test"
