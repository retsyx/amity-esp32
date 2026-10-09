#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Bounded test fixture for a Pi whose 2.4 GHz AP interferes with BLE setup.

An independently started AP helper must wait for --ap-trigger. This wrapper
stages credentials with the AP off, releases that helper at ConnectNetwork,
extends the live PASE failsafe, then starts CASE with the new interface address.
Run as root on the isolated Pi harness; all logs and credentials stay private.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--storage", type=Path, required=True)
    parser.add_argument("--credentials", type=Path, required=True)
    parser.add_argument("--trust-store", type=Path, required=True)
    parser.add_argument("--ap-trigger", type=Path, required=True)
    parser.add_argument("--attempt", required=True, help="Fresh alphanumeric result prefix")
    parser.add_argument("--expected-version", required=True)
    args = parser.parse_args()
    if not args.attempt.isalnum():
        parser.error("--attempt must be alphanumeric")
    os.umask(0o077)
    root = Path(__file__).resolve().parents[1]
    for path in (args.storage, args.credentials, args.trust_store, args.ap_trigger):
        if not path.resolve().is_relative_to(root):
            parser.error("All harness paths must be beneath esp32")
    status = args.storage / (args.attempt + ".network-ready")
    extended = Path(str(status) + ".extended")
    log_path = args.storage / (args.attempt + ".ble.log")
    case_log = args.storage / (args.attempt + ".case.log")
    if any(p.exists() for p in (status, extended, log_path, case_log, args.ap_trigger)):
        parser.error("Trigger, status and attempt log paths must be fresh")
    common = ["unshare", "--mount", "--propagation", "private", "env",
              "PYTHONDONTWRITEBYTECODE=1", "PYTHONPATH=" + str(root / ".matter-python"),
              "python3", str(root / "tests/matter_pi_controller.py"),
              "--storage", str(args.storage), "--trust-store", str(args.trust_store),
              "--isolate-platform-tmp", "--expect-unknown-audio",
              "--expected-version", args.expected_version]
    command = common + ["--credentials", str(args.credentials), "--handoff-status", str(status)]
    proc = None
    handed_off = False
    try:
        with log_path.open("xb") as log:
            proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 220
            with log_path.open("rb") as reader:
                tail = b""
                while proc.poll() is None:
                    tail += reader.read()
                    if not args.ap_trigger.exists() and b"SendCommand kWiFiNetworkEnable, supportsConcurrentConnection=true" in tail:
                        args.ap_trigger.write_text(json.dumps({"stage": "WiFiNetworkEnable", "monotonic_ns": time.monotonic_ns()}))
                        print("AP trigger released after WiFi credentials were staged", flush=True)
                    if not status.exists() and b"Received ConnectNetwork response, networkingStatus=0" in tail:
                        status.write_text(json.dumps({"network_connected": True}))
                        print("DUT network connected; extending commissioning failsafe", flush=True)
                    if extended.exists():
                        proc.terminate()
                        proc.wait(timeout=10)
                        handed_off = True
                        print("Failsafe extended; starting fresh operational CASE controller", flush=True)
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError("BLE staging exceeded 220 seconds")
                    tail = tail[-1024:]
                    time.sleep(0.1)
        if handed_off:
            with case_log.open("xb") as log:
                proc = subprocess.Popen(common + ["--complete-staged"], stdout=log, stderr=subprocess.STDOUT)
                proc.wait(timeout=65)
        print("Matter handoff controller exit", proc.returncode, flush=True)
        return 0 if proc.returncode == 0 else 1
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
