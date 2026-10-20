#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
json="$root/managed_components/espressif__cjson/cJSON"
test -f "$json/cJSON.c" || json="$root/managed_components/espressif__cjson"
openssl_include=""
openssl_lib=""
if test -d /opt/homebrew/opt/openssl@3; then
    openssl_include="-I/opt/homebrew/opt/openssl@3/include"
    openssl_lib="-L/opt/homebrew/opt/openssl@3/lib"
fi
cc -g -Wno-deprecated-declarations -fsanitize=address,undefined -I"$json" -c "$json/cJSON.c" -o "$out/local_system_cjson.o"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$root/components/config_store/include" -I"$root/components/amity_system/include" "$root/tests/identity_journal_test.cpp" -o "$out/identity_journal_test"
"$out/identity_journal_test"
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 $openssl_include -I"$json" -I"$root/tests/cec_hub_fakes" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" -I"$root/components/config_store/include" -I"$root/components/amity_system/include" "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/remote_control.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_hub/live_system.cpp" "$root/components/amity_system/coordinator.cpp" "$root/components/amity_system/model.cpp" "$root/components/amity_system/discovery.cpp" "$root/components/amity_system/planner.cpp" "$root/components/amity_system/policy.cpp" "$root/components/config_store/system_configuration.cpp" "$root/tests/cec_configuration_install_test.cpp" "$out/local_system_cjson.o" $openssl_lib -lcrypto -o "$out/cec_configuration_install_test"
"$out/cec_configuration_install_test"
