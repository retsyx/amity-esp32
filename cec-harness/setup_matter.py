#!/usr/bin/env python3
"""Install and verify the harness's pinned Raspberry Pi Matter controller SDK."""

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile


_SDK_COMMIT = "06523c22640ceb8b89f9a11ff2325a4481a178a3"  # Matter v1.4.2.0.
_TEST_ROOTS = {
    "Chip-Test-PAA-FFF1-Cert.der": "369eef40616645e5bd3e1bc9db70815d6cf93e9a611973ac20c9cd14c8d60c37",
    "Chip-Test-PAA-NoVID-Cert.der": "178632f8f235c0fa6a820de1d4c9b69568e2d16b7c2912b22f056136df75babc",
}
_NATIVE = "chip/_ChipDeviceCtrl.so"


@dataclass(frozen=True)
class Wheel:
    filename: str
    package: str
    version: str
    digest: str


def manifest(path):
    content = path.read_text(encoding="utf-8")
    pattern = re.compile(r"^# wheel: ([A-Za-z0-9_.-]+\.whl)\n"
                         r"([A-Za-z0-9_.-]+)==([A-Za-z0-9_.-]+) --hash=sha256:([a-f0-9]{64})$", re.MULTILINE)
    entries = [Wheel(*match.groups()) for match in pattern.finditer(content)]
    remaining = pattern.sub("", content)
    if (not entries or len({entry.filename for entry in entries}) != len(entries)
            or any(line.strip() and not line.startswith("#") for line in remaining.splitlines())):
        raise ValueError("Invalid pinned Matter wheel manifest")
    return entries


def _sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _url(url, *, host):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.hostname != host or parsed.username or parsed.password:
        raise ValueError("Unexpected dependency download location")
    return urllib.request.urlopen(url, timeout=60)


def download(url, target, digest, *, host, limit=256 * 1024 * 1024):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".matter-download-", dir=target.parent)
    try:
        total = 0
        with os.fdopen(fd, "wb") as output, _url(url, host=host) as source:
            while chunk := source.read(1024 * 1024):
                total += len(chunk)
                if total > limit:
                    raise ValueError("Matter dependency download exceeds its size limit")
                output.write(chunk)
        temporary = Path(temporary)
        if _sha256(temporary) != digest:
            raise ValueError("Downloaded dependency hash mismatch: " + target.name)
        os.chmod(temporary, 0o644)
        os.replace(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)


def wheels(entries, directory, *, offline=False):
    for entry in entries:
        target = directory / entry.filename
        if not target.exists():
            if offline:
                raise ValueError("Missing cached wheel: " + entry.filename)
            metadata_url = "https://pypi.org/pypi/{}/{}/json".format(entry.package, entry.version)
            with _url(metadata_url, host="pypi.org") as source:
                metadata = json.loads(source.read(10 * 1024 * 1024))
            candidates = [item for item in metadata["urls"] if item["filename"] == entry.filename]
            if len(candidates) != 1 or candidates[0]["digests"]["sha256"] != entry.digest:
                raise ValueError("Pinned wheel is missing from PyPI: " + entry.filename)
            download(candidates[0]["url"], target, entry.digest, host="files.pythonhosted.org")
        if not target.is_file() or _sha256(target) != entry.digest:
            raise ValueError("Cached wheel hash mismatch: " + entry.filename)
    print(f"Verified {len(entries)} exact Matter wheel hashes")


def _members(archive):
    for member in archive.infolist():
        path = PurePosixPath(member.filename)
        if (path.is_absolute() or ".." in path.parts or "\\" in member.filename
                or stat.S_ISLNK(member.external_attr >> 16)):
            raise ValueError("Unsafe wheel member")
        if not member.is_dir():
            yield member


def _relocated_native(data):
    for suffix in (b"factory.ini", b"config.ini", b"counters.ini"):
        old = b"/data/chip_" + suffix + b"\0"
        new = b"/tmp//chip_" + suffix + b"\0"
        if len(old) != len(new) or data.count(old) != 1 or data.count(new) != 0:
            raise ValueError("Unexpected native Matter SDK storage paths")
        data = data.replace(old, new)
    return data


def verify_installed(entries, directory, destination):
    # Later wheels own any overlapping package files, matching installation order.
    owners = {}
    for entry in entries:
        with zipfile.ZipFile(directory / entry.filename) as archive:
            owners.update((member.filename, entry.filename) for member in _members(archive))
    for entry in entries:
        with zipfile.ZipFile(directory / entry.filename) as archive:
            for member in _members(archive):
                if owners[member.filename] != entry.filename:
                    continue
                target = destination / member.filename
                if target.is_symlink() or not target.is_file():
                    raise ValueError("Missing installed Matter SDK file: " + member.filename)
                expected = archive.read(member)
                if member.filename == _NATIVE:
                    expected = _relocated_native(expected)
                if target.read_bytes() != expected:
                    raise ValueError("Installed Matter SDK differs from pinned wheel: " + member.filename)
    print("Verified installed Matter SDK, including its three relocated platform storage paths")


def install(entries, directory, destination):
    if destination.exists():
        try:
            verify_installed(entries, directory, destination)
            return
        except ValueError as error:
            # A changed pin replaces the whole SDK, never patching stale files.
            print("Replacing the installed Matter SDK: " + str(error))
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".matter-install-", dir=destination.parent))
    retired = None
    try:
        for entry in entries:
            with zipfile.ZipFile(directory / entry.filename) as archive:
                for member in _members(archive):
                    target = staging / member.filename
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.read(member))
        native = staging / _NATIVE
        native.write_bytes(_relocated_native(native.read_bytes()))
        verify_installed(entries, directory, staging)
        if destination.exists():
            retired = Path(tempfile.mkdtemp(prefix=".matter-retired-", dir=destination.parent)) / "sdk"
            os.rename(destination, retired)
        try:
            os.rename(staging, destination)
        except BaseException:
            if retired is not None:
                os.rename(retired, destination)
                retired = None
            raise
    finally:
        if staging.exists():
            shutil.rmtree(staging)
        if retired is not None:
            shutil.rmtree(retired.parent)


def trust_store(directory, *, offline=False):
    for name, digest in _TEST_ROOTS.items():
        target = directory / name
        if not target.exists():
            if offline:
                raise ValueError("Missing Matter test attestation root: " + name)
            url = f"https://raw.githubusercontent.com/project-chip/connectedhomeip/{_SDK_COMMIT}/credentials/test/attestation/{name}"
            download(url, target, digest, host="raw.githubusercontent.com", limit=65536)
        if not target.is_file() or _sha256(target) != digest:
            raise ValueError("Matter test attestation root hash mismatch: " + name)
    print("Verified official Matter test attestation roots; existing additional roots are retained")


def main(argv=None):
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheels", type=Path, default=project / ".matter-wheels")
    parser.add_argument("--destination", type=Path, default=project / ".matter-python")
    parser.add_argument("--trust-store", type=Path, default=project / "paa-root-certs")
    parser.add_argument("--check-wheels-only", action="store_true", help="verify cached wheels without downloading")
    parser.add_argument("--verify-installed", action="store_true", help="verify installation and roots without changing files")
    args = parser.parse_args(argv)
    try:
        entries = manifest(Path(__file__).with_name("matter-requirements.txt"))
        if not args.check_wheels_only and (platform.system() != "Linux" or platform.machine() != "aarch64"
                                         or sys.version_info[:2] != (3, 13)):
            raise ValueError("Pinned Matter controller requires Linux aarch64 and Python 3.13")
        wheels(entries, args.wheels, offline=args.check_wheels_only or args.verify_installed)
        if args.check_wheels_only:
            return 0
        if args.verify_installed:
            verify_installed(entries, args.wheels, args.destination)
        else:
            install(entries, args.wheels, args.destination)
        trust_store(args.trust_store, offline=args.verify_installed)
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
