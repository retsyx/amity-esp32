#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Install hash-checked CHIP test wheels locally, without pip or system changes."""
import argparse
import hashlib
from pathlib import Path
import platform
import re
import sys
import zipfile


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheels", type=Path, default=project / ".matter-wheels")
    parser.add_argument("--destination", type=Path, default=project / ".matter-python")
    parser.add_argument("--check-wheels-only", action="store_true")
    parser.add_argument("--verify-installed", action="store_true")
    args = parser.parse_args()
    lock = Path(__file__).with_name("matter_pi_requirements.txt").read_text()
    entries = re.findall(r"^# wheel: (\S+)\n\S+ --hash=sha256:([a-f0-9]{64})$", lock, re.MULTILINE)
    if len(entries) != 36:
        parser.error("Unexpected wheel manifest; review it before changing the installer")
    for filename, digest in entries:
        wheel = args.wheels / filename
        if hashlib.sha256(wheel.read_bytes()).hexdigest() != digest:
            parser.error("Wheel hash mismatch: " + filename)
    print(f"Verified {len(entries)} exact wheel hashes")
    if args.check_wheels_only:
        return
    if platform.machine() != "aarch64" or sys.version_info[:2] != (3, 13):
        parser.error("These pinned wheels require Linux aarch64 and Python 3.13")
    destination = args.destination.resolve()
    if not destination.is_relative_to(project):
        parser.error("Destination must remain beneath the esp32 project")
    if not args.verify_installed:
        if destination.exists():
            parser.error("Destination exists; use --verify-installed or choose a new local directory")
        destination.mkdir(parents=True)
        for filename, _ in entries:
            with zipfile.ZipFile(args.wheels / filename) as archive:
                for name in archive.namelist():
                    if not (destination / name).resolve().is_relative_to(destination):
                        parser.error("Unsafe wheel member: " + name)
                archive.extractall(destination)
    native = destination / "chip/_ChipDeviceCtrl.so"
    data = native.read_bytes()
    core_wheel = next(filename for filename, _ in entries if filename.startswith("home_assistant_chip_core-"))
    with zipfile.ZipFile(args.wheels / core_wheel) as archive:
        expected = archive.read("chip/_ChipDeviceCtrl.so")
    for suffix in (b"factory.ini", b"config.ini", b"counters.ini"):
        old = b"/data/chip_" + suffix + b"\0"
        new = b"/tmp//chip_" + suffix + b"\0"
        assert len(old) == len(new)
        if args.verify_installed:
            if data.count(old) != 0 or data.count(new) != 1:
                parser.error("Installed test SDK storage relocation does not match the manifest")
        else:
            if data.count(old) != 1 or data.count(new) != 0:
                parser.error("Unexpected native SDK storage paths; refusing to patch")
            data = data.replace(old, new)
        expected = expected.replace(old, new)
    if data != expected:
        parser.error("Native library differs from the locked wheel beyond the three storage paths")
    if not args.verify_installed:
        native.write_bytes(data)
    print("Verified three test-only platform storage paths use the private /tmp namespace")


if __name__ == "__main__":
    main()
