#!/usr/bin/env python3
"""Write an update bundle made by tools/update_bundle.py for the firmware upload test.

system_services_test parses and uploads this bundle with the firmware's own
code, so the firmware is checked against the release tool's layout rather
than against bytes the C++ test assembled itself.
"""
import hashlib
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
import hardware_profile  # noqa: E402
import update_bundle  # noqa: E402

image = bytearray(288)
image[0] = 0xe9
image[32:36] = bytes.fromhex('3254cdab')
image[176:208] = bytes(range(32))
image += hardware_profile.descriptor({'AMITY_BOARD_LEGACY_1T': 'y', 'AMITY_BOARD_DEVELOPMENT_4T': 'n'})
manifest = {'image_bytes': str(len(image)), 'image_sha256': hashlib.sha256(image).hexdigest(),
            'image_elf_sha256': image[176:208].hex(), 'signing_key_id': '',
            'hardware_profiles': ['esp32s3-legacy-1t-v1']}
canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
Path(sys.argv[1]).write_bytes(update_bundle.bundle(bytes(image), canonical))
