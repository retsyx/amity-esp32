#!/bin/sh
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Usage: sh tests/run_host_tests.sh [all|standalone|sdk]
# Builds and runs the host tests in parallel and reports every failure.
# standalone: tests that need no ESP-IDF checkout, managed components or
# firmware build output. sdk: only the tests that need them. all: both.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/test-results/host"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
cmake -S "$root/tests" -B "$build" -G Ninja -DAMITY_HOST_TESTS="${1:-all}" >/dev/null
status=0
# Keep building after a failed target. Its executable is missing, so its tests
# fail in the CTest summary instead of running a stale build.
rm -rf "$build/bin"
cmake --build "$build" --parallel "$jobs" -- -k 0 || status=$?
ctest --test-dir "$build" --output-on-failure --parallel "$jobs" || status=$?
exit "$status"
