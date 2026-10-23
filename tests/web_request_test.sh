#!/bin/sh
# Copyright 2026. This file is part of Amity-esp32, distributed under GPL-3.0-or-later.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
c++ -std=c++17 -Wall -Wextra -Werror -g -pthread -fsanitize=address,undefined \
  -I"$root/tests/web_request_fakes" -I"$root/components/web/include" -I"$root/components/system_services/include" \
  "$root/components/web/request.cpp" "$root/components/system_services/system_services.cpp" \
  "$root/tests/web_request_test.cpp" -o "$out/web_request_test"
"$out/web_request_test"
