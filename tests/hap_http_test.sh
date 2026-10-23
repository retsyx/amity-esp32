#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
hap="$root/third_party/esp-homekit-sdk/components/esp_hap_core/src"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/hap_http_fakes" -I"$hap/priv_includes" \
  "$hap/amity_hap_http.c" "$root/tests/hap_http_test.c" -o "$out/hap_http_test"
"$out/hap_http_test"
