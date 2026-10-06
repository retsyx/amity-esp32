#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
export GOCACHE="$root/.cache/go-build" GOMODCACHE="$root/.cache/go-mod"
(cd "$root/amity-provision" && go test ./... && go vet ./...)
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/device_settings/include" "$root/tests/factory_credentials_test.cpp" -o "$out/factory_test"
"$out/factory_test" "$root/amity-provision/testdata/factory-v1.hex" "$root/amity-provision/testdata/factory-v2.hex" "$root/amity-provision/testdata/factory-v3.hex"

c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/tests/reset_fakes" -I"$root/components/device_settings/include" \
  "$root/components/device_settings/factory_reset.cpp" "$root/tests/factory_reset_test.cpp" -o "$out/reset_test"
"$out/reset_test"
