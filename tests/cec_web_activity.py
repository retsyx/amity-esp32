#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Validate CEC activities through the integrated firmware's HTTPS API.

Run only against the authorized two-bus fixture after Wi-Fi onboarding.
Credentials are read from a mode-0600 boot log and never printed or included
in reports. Leaves eight fixture activities configured for BLE tests unless
--restore is given. Uses the self-signed fixture certificate by default; a
trusted certificate can be supplied with --cafile. Never opens a serial port.
"""
import argparse
import json
import os
import re
import shlex
import ssl
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from cec_hardware import Harness, raw_frames


def read_password(logs):
    for path in logs:
        if not path.exists():
            continue
        if stat.S_IMODE(path.stat().st_mode) & 0o077:
            raise RuntimeError(f"Boot log is not private: {path}; use mode 0600")
        matches = re.findall(r"AMITY INITIAL ADMIN PASSWORD:\s*([^\s\x1b]+)", path.read_text(errors="replace"))
        if matches:
            return matches[-1]
    raise RuntimeError("No initial admin password in the supplied private boot logs")


class Admin:
    def __init__(self, base, cafile=None):
        if not base.startswith("https://"):
            raise ValueError("The fixture API must use HTTPS")
        self.base = base.rstrip("/")
        self.context = ssl.create_default_context(cafile=str(cafile)) if cafile else ssl._create_unverified_context()
        self.token = None
        self.records = []

    def request(self, path, body=None):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(self.base + path, headers=headers,
                                         data=None if body is None else json.dumps(body).encode())
        started = time.monotonic()
        with urllib.request.urlopen(request, context=self.context, timeout=15) as response:
            content = json.load(response)
            self.records.append({"method": request.get_method(), "path": path,
                                 "status": response.status, "elapsed_s": round(time.monotonic() - started, 3)})
        return content

    def login(self, password):
        self.token = self.request("/api/login", {"password": password})["token"]

    def wait_status(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.request("/api/status")
            if predicate(state):
                return state
            time.sleep(.2)
        raise AssertionError("Timed out waiting for the requested hub state")

    def activity(self, index):
        self.request("/api/activity", {"index": index})
        return self.wait_status(lambda state: state["current_activity"] == index)


def write_private(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        json.dump(content, stream, indent=2)
        stream.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True, help="DUT HTTPS URL after Wi-Fi onboarding")
    parser.add_argument("--host", default="pi@10.0.1.119")
    parser.add_argument("--ssh-port", type=int, help="forwarded Pi SSH port, e.g. 2222")
    parser.add_argument("--boot-log", action="append", type=Path, help="private boot log; may be repeated")
    parser.add_argument("--cafile", type=Path)
    parser.add_argument("--restore", action="store_true", help="restore original activity configuration afterward")
    parser.add_argument("--output", type=Path, default=Path("test-results/cec-web-activity.json"))
    args = parser.parse_args()
    logs = args.boot_log or [Path("test-results/boot.log"), Path("test-results/boot-interrupt-failure.log")]
    admin = Admin(args.url, args.cafile)
    harness = Harness(args.host, args.ssh_port)
    report = {"passed": False, "http": admin.records, "cec": harness.records, "captures": {}}
    monitors = []
    original = None
    source_address_changed = False
    try:
        admin.login(read_password(logs))
        original = admin.request("/api/config")
        backup_path = args.output.with_name(args.output.stem + "-original-config.json")
        # A retry after installing the fixture must not overwrite the original
        # empty/user configuration with those eight temporary activities.
        if not backup_path.exists():
            write_private(backup_path, original)
        report["original_config_backup"] = str(backup_path)
        report["starting_activity_count"] = len(original.get("activities", []))
        fixture = {"version": 1, "activities": [
            {"name": f"Fixture {index + 1}", "display": "HarnessTV", "source": "HarnessSource", "audio": "HarnessSource"}
            for index in range(8)]}
        admin.request("/api/config", fixture)
        assert admin.request("/api/config") == fixture, "Activity config did not round-trip"
        admin.request("/api/discover", {})
        admin.wait_status(lambda state: state["discovering"])
        state = admin.wait_status(lambda state: not state["discovering"] and
                                  {"HarnessTV", "HarnessSource"}.issubset({d["name"] for d in state["devices"]}))
        assert state["ready"], "CEC adapters are not ready"
        expected_devices = {"HarnessTV": ("front", 0, 0, 0, 1),
                            "HarnessSource": ("back", 4, 0x1000, 4, 2)}
        for name, expected in expected_devices.items():
            device = next(d for d in state["devices"] if d["name"] == name)
            actual = tuple(device[field] for field in ("bus", "address", "physical_address", "type", "vendor_id"))
            assert actual == expected, f"Incomplete discovery metadata for {name}: {actual}"
        report["discovery"] = state
        front_address, back_address = state["front_address"], state["back_address"]
        for adapter in ("/dev/cec0", "/dev/cec1"):
            command = shlex.join(["sudo", "-n", "cec-ctl", "-d", adapter, "-s", "-r", "-v", "-M", "--monitor-time", "120"])
            process = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                                        "-o", "UserKnownHostsFile=/dev/null",
                                        *(["-p", str(args.ssh_port)] if args.ssh_port else []), args.host, command],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            monitors.append((adapter, process))
        time.sleep(.6)
        admin.activity(0)
        time.sleep(1)
        harness.expect("/dev/cec0", front_address, "--give-device-power-status", bytes([0x90, 0]))
        harness.expect("/dev/cec1", back_address, "--give-device-power-status", bytes([0x90, 0]))
        # Rescanning must retain the active source's old live mapping while
        # new descriptors are staged, including its truthful power reply.
        admin.request("/api/discover", {})
        scanning = admin.wait_status(lambda state: state["discovering"])
        assert scanning["current_activity"] == 0
        assert {"HarnessTV", "HarnessSource"}.issubset({d["name"] for d in scanning["devices"]})
        harness.expect("/dev/cec1", back_address, "--give-device-power-status", bytes([0x90, 0]))
        admin.wait_status(lambda state: not state["discovering"])
        # A live selected source moving to another physical HDMI input must
        # restore that route, then restore the fixture address for BLE tests.
        for physical, label in ((0x2000, "2.0.0.0"), (0x1000, "1.0.0.0")):
            source_address_changed = True
            harness.cec("/dev/cec1", "--phys-addr", label)
            admin.wait_status(lambda state: any(d["name"] == "HarnessSource" and d["physical_address"] == physical
                                                for d in state["devices"]))
        source_address_changed = False
        admin.request("/api/key", {"key": 0})
        admin.request("/api/key", {"key": 0x41})
        time.sleep(.6)
        # Every activity shares its display/source/audio. None should be sent
        # standby while switching through the eight BLE activity slots.
        for index in range(1, 8):
            admin.activity(index)
            time.sleep(.2)
        admin.activity(7)  # Reselect repairs power/routing.
        time.sleep(1)
        admin.activity(-1)
        time.sleep(.7)
        harness.expect("/dev/cec0", front_address, "--give-device-power-status", bytes([0x90, 1]))
        harness.expect("/dev/cec1", back_address, "--give-device-power-status", bytes([0x90, 1]))
        harness.cec("/dev/cec1", "--active-source", "phys-addr=1.0.0.0")
        time.sleep(.5)
        for adapter, process in monitors:
            stdout, stderr = process.communicate(timeout=125)
            report["captures"][adapter] = stdout
            if process.returncode or "Selecting monitor mode failed" in stderr:
                raise RuntimeError(stderr)
        front = raw_frames(report["captures"]["/dev/cec0"])
        back = raw_frames(report["captures"]["/dev/cec1"])
        front_direct = front_address << 4
        back_direct = (back_address << 4) | 4
        assert front.count(bytes([front_direct, 0x04])) >= 2, "Missing display wake or activity repair"
        assert front.count(bytes([front_direct, 0x36])) == 1, "Shared display powered off during an activity switch"
        assert bytes([front_direct | 15, 0x36]) in front, "Missing forced front standby after source theft"
        for payload in ([0x44, 0x6d], [0x45], [0x44, 0], [0x44, 0x41], [0x36]):
            assert bytes([back_direct, *payload]) in back, f"Missing back payload {bytes(payload).hex(':')}"
        assert bytes([(back_address << 4) | 15, 0x86, 0x10, 0]) in back, "Missing Set Stream Path"
        assert bytes([(back_address << 4) | 15, 0x86, 0x20, 0]) in back, "Missing routing repair after source address change"
        assert bytes([(back_address << 4) | 15, 0x36]) in back, "Missing forced back standby after source theft"
        report["final_status"] = admin.request("/api/status")
        assert report["final_status"]["current_activity"] == -1
        report["passed"] = True
        print("PASS: HTTPS configuration, staged discovery, eight activities, route repair, key routing, power feedback, and forced standby")
        return 0
    except (AssertionError, RuntimeError, ValueError, OSError, urllib.error.URLError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        for adapter, process in monitors:
            if process.poll() is None:
                process.terminate()
                stdout, _ = process.communicate(timeout=5)
                report["captures"].setdefault(adapter, stdout)
        if source_address_changed:
            try:
                harness.cec("/dev/cec1", "--phys-addr", "1.0.0.0")
            except Exception:
                report["source_address_restore_failed"] = True
        if admin.token:
            try:
                admin.activity(-1)
                if args.restore and original is not None:
                    admin.request("/api/restore", original)
                admin.request("/api/logout", {})
            except Exception:
                report["cleanup_incomplete"] = True
        write_private(args.output, report)


if __name__ == "__main__":
    sys.exit(main())
