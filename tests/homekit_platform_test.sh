#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
platform="$root/third_party/esp-homekit-sdk/components/esp_hap_platform"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -g -fsanitize=address,undefined \
  -I"$root/tests/homekit_platform_fakes" -c "$platform/src/hap_platform_httpd.c" -o "$out/homekit_platform_httpd.o"
for encryption in plain encrypted; do
  encrypted_flag=
  if [ "$encryption" = encrypted ]; then encrypted_flag=-DCONFIG_NVS_ENCRYPTION=1; fi
  c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread $encrypted_flag \
    -I"$root/tests/homekit_platform_fakes" -I"$platform/include" \
    "$platform/src/hap_platform_keystore.cpp" "$root/tests/homekit_platform_test.cpp" \
    "$out/homekit_platform_httpd.o" -o "$out/homekit_platform_test_$encryption"
  "$out/homekit_platform_test_$encryption"
done
