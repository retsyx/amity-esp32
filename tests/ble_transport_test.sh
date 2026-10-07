#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/ble_transport_fakes" -I"$root/components/ble_remote/include" \
  "$root/tests/ble_transport_test.cpp" "$root/components/ble_remote/hid_keyboard.cpp" \
  "$root/components/ble_remote/siri_remote.cpp" -o "$out/ble_transport_test"
for scenario in absence success unpair unpair-before-completion scan scan-unpair \
  cancel-error unpair-cancel-error connect-error async-error discovery-error \
  disconnect-before-setup host-reset pairing; do
  "$out/ble_transport_test" "$scenario"
done
