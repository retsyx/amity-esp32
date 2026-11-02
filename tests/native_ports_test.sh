#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results/host"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/tests/cec_hub_fakes" -I"$root/components/amity_system/include" -I"$root/components/config_store/include" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
    "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" \
    "$root/components/amity_system/model.cpp" \
    "$root/tests/native_ports_test.cpp" -o "$root/test-results/host/native_ports_test"
"$root/test-results/host/native_ports_test"
