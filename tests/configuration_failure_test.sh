#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "$root/test-results/configuration-failure.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
json="$root/managed_components/espressif__cjson/cJSON"
test -f "$json/cJSON.c" || json="$root/managed_components/espressif__cjson"
openssl_include=""
openssl_lib=""
if test -d /opt/homebrew/opt/openssl@3; then
    openssl_include="-I/opt/homebrew/opt/openssl@3/include"
    openssl_lib="-L/opt/homebrew/opt/openssl@3/lib"
fi
cc -g -Wno-deprecated-declarations -fsanitize=address,undefined -I"$json" -c "$json/cJSON.c" -o "$out/cjson.o"
# Rename only this translation unit's digest entrypoint; the test wrapper calls
# the real SHA256 implementation whenever the selected failure is not reached.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    $openssl_include -I"$json" -I"$root/tests/reset_fakes" -I"$root/components/config_store/include" -I"$root/components/amity_system/include" \
    -DSHA256=amity_failure_sha256 -c "$root/components/config_store/system_configuration.cpp" -o "$out/configuration.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    $openssl_include -I"$json" -I"$root/tests/reset_fakes" -I"$root/components/config_store/include" -I"$root/components/amity_system/include" \
    "$root/components/amity_system/model.cpp" "$root/components/config_store/configuration_journal.cpp" \
    "$root/tests/configuration_failure_test.cpp" "$out/configuration.o" "$out/cjson.o" $openssl_lib -lcrypto -o "$out/test"
"$out/test"
