#!/usr/bin/env bash
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

# Create the GitHub release for RELEASE_TAG in GH_REPO from the assets in dist/.
set -euo pipefail
cat > release-notes.md <<EOF
## Install Amity

For a fresh installation, download **amity-firmware.zip** and the
**amity-provision** ZIP for your computer from the assets below.
Extract the provisioner and use it to create your device's firmware
image and Matter pairing card before flashing over USB.

See the [installation guide](https://github.com/$GH_REPO/blob/$RELEASE_TAG/README.md)
for hardware requirements, downloads, USB flashing and first setup.
The same guide is attached as **README.md** and included in each provisioner ZIP.

To update an installed Amity, download **amity-update.bin** and upload that one file in
**Advanced → Update Firmware**. Normal updates preserve settings and pairings.
Before version 1.0, backward compatibility with earlier releases is not
maintained. Read this version's release notes before updating; a fresh
installation and pairing again may be required.
A fresh-install image replaces existing configuration and pairings.

The update file includes its exact release manifest and, for signed
releases, publisher signature. **amity.bin** remains the raw application
for USB/debug tooling; the full installation image is not an OTA file.

**SHA256SUMS** contains checksums for the downloadable assets.
GitHub's automatic source archives are for building from source.
EOF
flags=(--verify-tag --generate-notes --notes-file release-notes.md --title "$RELEASE_TAG")
version="${RELEASE_TAG%%+*}"
if [[ "$version" == *-* ]]; then
  flags+=(--prerelease)
fi
assets=(dist/amity-update.bin dist/amity.release.json dist/amity.bin
        dist/amity.elf dist/amity.map dist/amity-firmware.zip
        dist/amity-provision-*.zip dist/README.md dist/SHA256SUMS)
if [[ -f dist/amity.release.sig ]]; then
  assets+=(dist/amity.release.sig)
fi
gh release create "$RELEASE_TAG" "${assets[@]}" "${flags[@]}"
