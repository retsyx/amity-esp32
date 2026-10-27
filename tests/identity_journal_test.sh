#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/test-results/host"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
 -I"$root/components/config_store/include" -I"$root/components/amity_system/include" "$root/tests/identity_journal_test.cpp" -o "$root/test-results/host/identity_journal_test"
"$root/test-results/host/identity_journal_test"
