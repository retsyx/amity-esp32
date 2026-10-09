#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Build a checksummed, unprovisioned release package from an ESP-IDF build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


def package(build, output):
    config = (build / 'config/sdkconfig.h').read_text()
    def setting(name):
        match = re.search(r'^#define ' + name + r' (\S+)$', config, re.M)
        if not match:
            raise ValueError(f'Missing {name}')
        return int(match[1], 0)
    description = json.loads((build / 'project_description.json').read_text())
    if description['target'] != 'esp32s3' or description['project_name'] != 'amity':
        raise ValueError('Only the Amity ESP32-S3 build is supported')
    mapping = [
        ('bootloader.bin', 'bootloader/bootloader.bin', 0),
        ('partition-table.bin', 'partition_table/partition-table.bin', 0xc000),
        ('ota-data.bin', 'ota_data_initial.bin', 0x21000),
        ('amity.bin', 'amity.bin', 0x40000),
    ]
    manifest = dict(format='amity-firmware', version=3, firmware_version=description['project_version'],
                    chip='esp32s3', flash_size=0x800000, factory_offset=0xf000, factory_size=0x1000,
                    vendor_id=setting('CONFIG_DEVICE_VENDOR_ID'), product_id=setting('CONFIG_DEVICE_PRODUCT_ID'), images=[])
    images = {}
    for name, source, offset in mapping:
        data = (build / source).read_bytes()
        images[name] = data
        manifest['images'].append(dict(path=name, offset=offset, sha256=hashlib.sha256(data).hexdigest()))
    # Ensure this build actually contains the provisioning partition, not an older table.
    table = images['partition-table.bin']
    import struct
    entries = [struct.unpack('<HBBII16sI', table[i:i+32]) for i in range(0, len(table)-31, 32)]
    if not any(e[:5] == (0x50aa, 1, 0x40, 0xf000, 0x1000) and e[5].rstrip(b'\0') == b'amity_factory' and e[6] == 0 for e in entries):
        raise ValueError('Missing unencrypted factory partition')
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.zip.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(images.items()):
            z.writestr(name, data)
        z.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
        project = Path(__file__).resolve().parents[1]
        for source in ('LICENSE', 'THIRD_PARTY.md', 'third_party/esp-homekit-sdk/LICENSES.txt'):
            z.write(project / source, arcname='notices/' + Path(source).name)
    temporary.replace(output)
    print(f'Firmware package: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    package(args.build, args.output or args.build / 'amity-firmware.zip')
