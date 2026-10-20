#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "$root/test-results/replacement.XXXXXX")
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
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    $openssl_include -I"$json" -I"$root/tests/reset_fakes" -I"$root/components/config_store/include" -I"$root/components/amity_system/include" -I"$root/components/system_sync/include" -I"$root/components/consortium/include" \
    "$root/components/amity_system/model.cpp" "$root/components/config_store/system_configuration.cpp" \
    "$root/components/config_store/configuration_journal.cpp" "$root/components/config_store/configuration_migration.cpp" "$root/components/config_store/owner_configuration.cpp" "$root/components/config_store/coordinator_intent.cpp" \
    "$root/components/consortium/json.cpp" "$root/components/consortium/wire.cpp" "$root/components/consortium/settings.cpp" "$root/components/consortium/relay.cpp" "$root/components/consortium/update.cpp" "$root/components/consortium/control.cpp" "$root/components/consortium/transfer.cpp" "$root/components/system_sync/synchronization_codec.cpp" \
    "$root/components/config_store/configuration_replacement.cpp" "$root/tests/configuration_replacement_test.cpp" "$out/cjson.o" $openssl_lib -lcrypto -o "$out/test"
"$out/test"
