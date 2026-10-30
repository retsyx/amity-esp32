#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
json="$root/managed_components/espressif__cjson/cJSON"
test -f "$json/cJSON.c" || json="$root/managed_components/espressif__cjson"
cc -g -Wno-deprecated-declarations -fsanitize=address,undefined -I"$json" -c "$json/cJSON.c" -o "$out/input_cjson.o"
openssl_include=""; openssl_lib=""
if test -d /opt/homebrew/opt/openssl@3; then
 openssl_include="-I/opt/homebrew/opt/openssl@3/include"; openssl_lib="-L/opt/homebrew/opt/openssl@3/lib"
fi
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 $openssl_include -I"$json" \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/config_store/include" -I"$root/components/cec/include" \
 -I"$root/components/amity_hub/include" -I"$root/components/amity_system/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" \
 "$root/components/amity_hub/system_control.cpp" "$root/components/amity_hub/remote_control.cpp" "$root/components/config_store/system_configuration.cpp" \
 "$root/components/config_store/configuration_journal.cpp" \
 "$root/components/amity_system/model.cpp" \
 "$root/tests/system_remote_control_test.cpp" "$out/input_cjson.o" \
 $openssl_lib -lcrypto -o "$out/system_remote_control_test"
"$out/system_remote_control_test"
