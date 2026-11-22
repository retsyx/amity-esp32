#!/usr/bin/env python3
"""Collect the downloadable release assets and their checksums in build/release.

Run from the repository root after the firmware and provisioner builds.
RELEASE_TAG is the release tag, or empty for an ordinary build. A tag must
match the built firmware version; the attached README.md then links to that
tag instead of main.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import zipfile


def main():
    build = Path('build')
    with zipfile.ZipFile(build / 'amity-firmware.zip') as package:
        version = json.loads(package.read('manifest.json'))['firmware_version']
    tag = os.environ['RELEASE_TAG']
    if tag and tag != 'v' + version:
        raise SystemExit('Built firmware version does not match release tag')
    sources = [build / name for name in
               ('amity.bin', 'amity-update.bin', 'amity.release.json', 'amity.elf', 'amity.map', 'amity-firmware.zip')]
    if (build / 'amity.release.sig').exists():
        sources.append(build / 'amity.release.sig')
    sources.append(Path('README.md'))
    sources.extend(build / 'amity-provision' / f'amity-provision-{system}-{arch}.zip'
                   for system in ('darwin', 'windows', 'linux')
                   for arch in ('amd64', 'arm64'))
    destination = build / 'release'
    destination.mkdir(exist_ok=True)
    checksums = []
    for source in sorted(sources, key=lambda path: path.name):
        target = destination / source.name
        shutil.copyfile(source, target)
        if tag and source == Path('README.md'):
            guide = target.read_bytes()
            for kind in ('blob', 'tree'):
                guide = guide.replace(
                    f'https://github.com/retsyx/amity-esp32/{kind}/main/'.encode(),
                    f'https://github.com/retsyx/amity-esp32/{kind}/{tag}/'.encode())
            target.write_bytes(guide)
        checksums.append(f'{hashlib.sha256(target.read_bytes()).hexdigest()}  {target.name}')
    (destination / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')


if __name__ == '__main__':
    main()
