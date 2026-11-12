#!/usr/bin/env python3
"""Install and verify the Pi's functional-test Python and browser runtime."""

import argparse
from importlib import metadata
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


HARNESS = Path(__file__).resolve().parent
DESTINATION = HARNESS.parent / ".harness-python"
REQUIREMENTS = HARNESS / "runtime-requirements.txt"
CONSTRAINTS = HARNESS / "runtime-constraints.txt"


def validate_platform():
    if (platform.system() != "Linux" or platform.machine() != "aarch64"
            or sys.version_info[:2] != (3, 13)):
        raise ValueError("The harness runtime requires Linux aarch64 and Python 3.13")


def pinned_requirements(path):
    entries = []
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = re.fullmatch(r"([A-Za-z0-9_.-]+)==([A-Za-z0-9_.-]+)", line.strip())
        if match is None:
            raise ValueError("Runtime requirements must pin exact package versions")
        entries.append(match.groups())
    if not entries or len({name.lower().replace("_", "-") for name, _ in entries}) != len(entries):
        raise ValueError("Runtime requirements must contain unique pinned packages")
    return entries


def pinned_constraints():
    """Every runtime requirement must be pinned identically in the full closure."""
    requirements, constraints = pinned_requirements(REQUIREMENTS), pinned_requirements(CONSTRAINTS)
    closure = {name.lower().replace("_", "-"): version for name, version in constraints}
    if any(closure.get(name.lower().replace("_", "-")) != version for name, version in requirements):
        raise ValueError("runtime-constraints.txt must pin every runtime requirement at the same version")
    return constraints


def verify_environment():
    """Check imports and a blank browser; never open or operate DUT devices."""
    for package, expected in pinned_constraints():
        if metadata.version(package) != expected:
            raise ValueError(f"Installed {package} differs from runtime-constraints.txt; rerun setup")
    from serial import Serial
    from serial.tools.list_ports import comports
    from esp_pylib.serial_reset import hard_reset
    from esptool import main as esptool_main
    import dbus_next
    from aiohomekit.controller.ip.controller import IpController
    from playwright.sync_api import sync_playwright

    if not all(callable(item) for item in (Serial, comports, hard_reset)):
        raise ValueError("USB reboot dependencies are unavailable; rerun setup")
    if not callable(esptool_main):
        raise ValueError("USB flashing dependency is unavailable; rerun setup")
    if not callable(IpController):
        raise ValueError("HomeKit controller dependency is unavailable; rerun setup")
    chromium = shutil.which("chromium") or shutil.which("chromium-browser")
    if chromium is None:
        raise ValueError("System Chromium is missing; rerun harness setup")
    try:
        with sync_playwright() as driver:
            browser = driver.chromium.launch(executable_path=chromium, headless=True, timeout=30000)
            browser.close()
    except Exception as error:
        raise RuntimeError("Headless Chromium verification failed: " + str(error)) from error
    print("Harness runtime verified: USB reboots and flashing, BLE, HomeKit and headless Chromium.")


def verify_installed(destination):
    python = destination / "bin/python"
    configuration = destination / "pyvenv.cfg"
    if not python.is_file() or not configuration.is_file():
        raise ValueError("Harness runtime is missing; rerun harness setup")
    if not re.search(r"^include-system-site-packages\s*=\s*true\s*$",
                     configuration.read_text(), re.MULTILINE | re.IGNORECASE):
        raise ValueError("Harness runtime needs system packages for BLE; rerun harness setup")
    subprocess.run([str(python), "-B", str(Path(__file__).resolve()), "--verify-environment"],
                   check=True, timeout=60)


def install(destination):
    pinned_constraints()
    subprocess.run([sys.executable, "-m", "venv", "--system-site-packages", str(destination)], check=True)
    # Constraints also pin transitive packages, overriding system versions.
    subprocess.run([str(destination / "bin/python"), "-m", "pip", "install",
                    "--disable-pip-version-check", "--requirement", str(REQUIREMENTS),
                    "--constraint", str(CONSTRAINTS)], check=True)
    verify_installed(destination)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify-installed", action="store_true",
                      help="verify the existing runtime without installing or accessing DUTs")
    mode.add_argument("--verify-environment", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        validate_platform()
        if args.verify_environment:
            verify_environment()
        elif args.verify_installed:
            verify_installed(DESTINATION)
        else:
            install(DESTINATION)
    except (OSError, ValueError, RuntimeError, ImportError, metadata.PackageNotFoundError,
            subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print("Harness runtime: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
