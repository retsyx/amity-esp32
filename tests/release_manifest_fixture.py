#!/usr/bin/env python3
"""Write the canonical release manifest fixture read by the C++ parser tests.

An independent Python generator supplies the exact canonical bytes, so the
tests do not merely serialize with the same codec twice.
"""
import importlib.util
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_manifest', root / 'tools/release_manifest.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
value = dict(format=1, release_id='test-release', firmware_version='0.1.55', image_bytes='4096',
             image_sha256='a' * 64, image_elf_sha256='b' * 64, hardware_profiles=['esp32s3-legacy-1t-v1'],
             signing_key_id='')
Path(sys.argv[1]).write_bytes(module.canonical(value))
