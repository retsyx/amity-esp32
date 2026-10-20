#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results/host"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 -I"$root/tests/cec_hub_fakes" -I"$root/components/config_store/include" -I"$root/components/cec/include" \
 -I"$root/components/amity_hub/include" -I"$root/components/amity_system/include" \
 "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" "$root/components/amity_hub/live_system.cpp" \
 "$root/components/amity_system/model.cpp" "$root/components/amity_system/discovery.cpp" "$root/components/amity_system/policy.cpp" \
 "$root/components/amity_system/planner.cpp" "$root/components/amity_system/coordinator.cpp" "$root/tests/live_system_test.cpp" -o "$root/test-results/host/live_system_test"
"$root/test-results/host/live_system_test"
