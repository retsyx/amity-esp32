#!/bin/sh
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/ble_transport_fakes" -I"$root/components/ble_remote/include" \
  "$root/tests/ble_transport_test.cpp" "$root/components/ble_remote/hid_keyboard.cpp" \
  "$root/components/ble_remote/siri_remote.cpp" "$root/components/ble_remote/remote_input.cpp" -o "$out/ble_transport_test"
for scenario in absence success unpair unpair-before-completion scan scan-unpair unpair-scan \
  cancel-error unpair-cancel-error connect-error async-error discovery-error \
  disconnect-before-setup host-reset pairing; do
  "$out/ble_transport_test" "$scenario"
done
