#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sh "$root/tests/release_manifest_test.sh"
out="$root/test-results/host"
openssl_include=""; openssl_lib=""
if test -d /opt/homebrew/opt/openssl@3; then
 openssl_include="-I/opt/homebrew/opt/openssl@3/include"; openssl_lib="-L/opt/homebrew/opt/openssl@3/lib"
fi
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined $openssl_include \
 -I"$root/tests/cohort_fakes" -I"$root/tests/cec_hub_fakes" -I"$root/tests/reset_fakes" \
 -I"$root/components/consortium/include" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/system_services/include" \
 "$root/components/system_services/group_firmware_update.cpp" "$root/components/system_services/release_manifest.cpp" \
 "$root/components/consortium/json.cpp" "$root/components/consortium/wire.cpp" "$root/components/consortium/settings.cpp" "$root/components/consortium/update.cpp" "$root/components/consortium/owned_intent.cpp" "$root/components/consortium/transfer.cpp" \
 "$root/components/amity_system/model.cpp" "$root/tests/cohort_update_test.cpp" $openssl_lib -lcrypto -o "$out/cohort_update_test"
"$out/cohort_update_test" "$out/release-manifest.json"
