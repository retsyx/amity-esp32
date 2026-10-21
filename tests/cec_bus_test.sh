#!/bin/sh
# Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# The consortium driver retains the same contract plus its shared/native
# dispatcher. Use its real-driver fixture when this script is merged there.
if [ -f "$root/components/cec/scheduler.cpp" ] && [ -f "$root/tests/cec_runtime_test.sh" ]; then
    exec sh "$root/tests/cec_runtime_test.sh"
fi
mkdir -p "$root/test-results"
binary=$(mktemp "$root/test-results/cec-bus.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$root/tests/cec_bus_fakes" -I"$root/components/cec/include" \
    "$root/components/cec/engine.cpp" "$root/components/cec/bus.cpp" \
    "$root/tests/cec_bus_test.cpp" -o "$binary"
"$binary"
