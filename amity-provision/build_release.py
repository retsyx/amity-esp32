#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Build standalone provisioning executables and release ZIPs (requires Go and Python)."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

root = Path(__file__).resolve().parent
esp32 = root.parent
out = esp32 / 'build/amity-provision'
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, CGO_ENABLED='0', GOCACHE=str(esp32 / '.cache/go-build'),
           GOMODCACHE=str(esp32 / '.cache/go-mod'))
subprocess.run(['go', 'test', './...'], cwd=root, env=env, check=True)
notice = (
    'amity-provision is part of Amity and distributed under the repository\'s '
    'GPLv3 license (see LICENSE).\n'
    'This executable includes the following third-party software and fonts.\n'
)
modules = subprocess.check_output(['go', 'list', '-deps', '-f', '{{with .Module}}{{.Path}} {{.Dir}}{{end}}', '.'], cwd=root, env=env).decode().splitlines()
for entry in sorted(set(filter(None, modules))):
    name, directory = entry.split(' ', 1)
    if Path(directory) == root:
        continue
    notice += '\n' + name + '\n' + (Path(directory) / 'LICENSE').read_text()
    if name == 'golang.org/x/image':
        notice += '\nGo fonts\n' + (Path(directory) / 'font/gofont/ttfs/README').read_text()
goroot = subprocess.check_output(['go', 'env', 'GOROOT'], cwd=root, env=env).decode().strip()
go_license = next(p for p in (Path(goroot) / 'LICENSE', Path(goroot).parent / 'LICENSE') if p.is_file())
notice += '\nGo runtime and standard library\n' + go_license.read_text()
readme = (esp32 / 'README.md').read_bytes()
release_tag = os.environ.get('RELEASE_TAG', '')
if release_tag:
    for kind in ('blob', 'tree'):
        readme = readme.replace(
            f'https://github.com/retsyx/amity-esp32/{kind}/main/'.encode(),
            f'https://github.com/retsyx/amity-esp32/{kind}/{release_tag}/'.encode())
checksums = []
for system in ('darwin', 'windows', 'linux'):
    for arch in ('amd64', 'arm64'):
        name = f'amity-provision-{system}-{arch}'
        directory = out / name
        directory.mkdir(exist_ok=True)
        executable = directory / ('amity-provision.exe' if system == 'windows' else 'amity-provision')
        subprocess.run(['go', 'build', '-trimpath', '-ldflags=-s -w', '-o', str(executable), '.'],
                       cwd=root, env=dict(env, GOOS=system, GOARCH=arch), check=True)
        (directory / 'README.md').write_bytes(readme)
        shutil.copyfile(esp32 / 'LICENSE', directory / 'LICENSE')
        (directory / 'NOTICE.txt').write_text(notice)
        archive = out / (name + '.zip')
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for path in sorted((executable, directory / 'README.md', directory / 'LICENSE',
                                directory / 'NOTICE.txt')):
                z.write(path, path.name)
        checksums.append(f'{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}')
        print(archive)
(out / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
