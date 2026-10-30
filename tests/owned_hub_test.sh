#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp "$root/test-results/owned-hub.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/tests/owned_hub_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/tests/owned_configuration_revision_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/tests/cec_scan_policy_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/tests/owned_source_theft_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=5 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/tests/owned_hub_settling_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" -I"$root/components/ble_remote/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/components/ble_remote/remote_input.cpp" "$root/components/ble_remote/siri_remote.cpp" "$root/tests/activity_sequence_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_system/model.cpp" "$root/tests/cec_activity_sequence_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
 -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_system/model.cpp" "$root/tests/device_announcement_test.cpp" -o "$binary"
"$binary"
