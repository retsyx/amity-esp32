#!/bin/sh
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results"
binary=$(mktemp "$root/test-results/cec-hub.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/components/cec/include" -I"$root/components/amity_hub/include" -I"$root/main" \
    "$root/components/cec/engine.cpp" "$root/components/amity_hub/hub.cpp" \
    "$root/tests/cec_hub_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 -I"$root/tests/cec_hub_fakes" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
    "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" \
    "$root/tests/cec_hub_commit_test.cpp" -o "$binary"
"$binary"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=0 \
    -I"$root/tests/cec_hub_fakes" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
    "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" \
    "$root/tests/cec_front_physical_test.cpp" -o "$binary"
"$binary"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
    "$root/tests/activity_settling_test.cpp" -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DCONFIG_AMITY_ACTIVITY_SETTLE_SECONDS=5 \
    -I"$root/tests/cec_hub_fakes" -I"$root/components/cec/include" -I"$root/components/amity_hub/include" \
    "$root/components/amity_hub/hub.cpp" "$root/components/amity_hub/hub_service.cpp" \
    "$root/tests/cec_hub_settling_test.cpp" -o "$binary"
"$binary"
