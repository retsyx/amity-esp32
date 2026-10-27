#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/test-results/host"
mkdir -p "$out"
# Independent Python generator supplies the exact canonical fixture consumed by
# the target C++ parser; tests do not merely serialize with the same codec twice.
python3 - "$root" "$out/release-manifest.json" <<'PY'
import importlib.util,sys
from pathlib import Path
root=Path(sys.argv[1]);spec=importlib.util.spec_from_file_location('release_manifest',root/'tools/release_manifest.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
value=dict(format=1,release_id='test-release',firmware_version='0.1.55',image_bytes='4096',image_sha256='a'*64,image_elf_sha256='b'*64,chip='esp32s3',hardware_profiles=['esp32s3-legacy-1t-v1'],partition_layout='amity-8m-ota3d0000-v1',slot_bytes=str(0x3d0000),protocol=dict(min='1',max='1',required_capabilities='401472',optional_capabilities='511'),configuration=dict(read_min='1',read_max='3',write='3'),storage=dict(read_min='1',read_max='2',write='2'),origin=dict(read_min='1',read_max='1',write='1'),factory=dict(read_min='1',read_max='4',write='4'),bootloader='esp-idf6.1-rollback-v1',security_profile='development',rollback_readable_max='2',source_commit='c'*40,source_tree_sha256='d'*64,source_dirty=True,dependencies=module.DEPENDENCIES,signing_key_id='')
Path(sys.argv[2]).write_bytes(module.canonical(value))
PY
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/components/consortium/include" -I"$root/components/amity_system/include" -I"$root/components/system_services/include" \
  "$root/components/system_services/release_manifest.cpp" "$root/components/consortium/json.cpp" \
  "$root/components/consortium/update.cpp" "$root/components/consortium/wire.cpp" "$root/components/consortium/settings.cpp" "$root/components/consortium/relay.cpp" "$root/components/consortium/owned_intent.cpp" "$root/components/consortium/transfer.cpp" \
  "$root/components/amity_system/model.cpp" "$root/tests/release_manifest_test.cpp" -o "$out/release_manifest_test"
"$out/release_manifest_test" "$out/release-manifest.json"
