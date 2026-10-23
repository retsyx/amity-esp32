#!/bin/sh
# Copyright 2026. Licensed under GPL-3.0-or-later.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
core="$root/third_party/esp-homekit-sdk/components/esp_hap_core/src"
mkdir -p "$out"
for source in esp_hap_pair_common esp_hap_pairings; do
  cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I"$root/tests/homekit_input_fakes" -I"$core/priv_includes" \
    -c "$core/$source.c" -o "$out/homekit_$source.o"
done
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/homekit_input_fakes" -I"$core/priv_includes" \
  "$root/tests/homekit_input_test.cpp" "$out/homekit_esp_hap_pair_common.o" \
  "$out/homekit_esp_hap_pairings.o" -o "$out/homekit_input_test"
"$out/homekit_input_test"
