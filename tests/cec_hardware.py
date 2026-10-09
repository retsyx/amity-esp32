#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Exercise the flashed ESP32 against Linux cec-gpio on the authorized harness.

This configures only the listed Pi CEC adapters as synthetic TV/source devices.
It never opens a serial port or changes DUT configuration. Keep real HDMI
devices disconnected from this two-wire fixture. Adapter assignments remain
configured so the web device-discovery flow can use HarnessTV/HarnessSource.

  python3 tests/cec_hardware.py --host pi@10.0.1.119

Use --inspect to print adapter state without configuring or transmitting.
"""
import argparse
import concurrent.futures
import json
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path


def raw_frames(output):
    return [bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", line))
            for line in output.splitlines() if "Raw:" in line]


class Harness:
    def __init__(self, host, port=None):
        self.host = host
        self.port = port
        self.records = []

    def command(self, args, timeout=20):
        command = shlex.join(args)
        start = time.monotonic()
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o",
             "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
             *(["-p", str(self.port)] if self.port else []), self.host, command],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
        )
        self.records.append({"command": command, "elapsed_s": round(time.monotonic() - start, 3),
                             "returncode": result.returncode, "output": result.stdout,
                             "stderr": result.stderr})
        if result.returncode:
            raise RuntimeError(f"{command}: {result.stderr or result.stdout}")
        return result.stdout

    def cec(self, adapter, *options):
        return self.command(["cec-ctl", "-d", adapter, "-s", "-r", "-v", *map(str, options)])

    def poll(self, adapter, address):
        output = self.cec(adapter, "-t", address, "--poll")
        return bool(re.search(r"Tx,\s*OK", output))

    def expect(self, adapter, address, option, payload):
        output = self.cec(adapter, "-t", address, option)
        replies = [f for f in raw_frames(output) if f and f[0] >> 4 == address]
        if not any(f[1:] == payload for f in replies):
            raise AssertionError(f"{adapter} addr {address} {option}: expected {payload.hex(':')};\n{output}")
        if "Rx, OK" not in output:
            raise AssertionError(f"{adapter} {option}: missing successful receive status\n{output}")
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="pi@10.0.1.119")
    parser.add_argument("--ssh-port", type=int, help="forwarded Pi SSH port, e.g. 2222")
    parser.add_argument("--adapters", nargs=2, default=["/dev/cec0", "/dev/cec1"])
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument("--iterations", type=int, default=10, help="concurrent requests per bus after identity tests")
    parser.add_argument("--output", type=Path, help="write full commands and outputs as JSON")
    args = parser.parse_args()
    harness = Harness(args.host, args.ssh_port)
    report = {"host": args.host, "passed": False, "commands": harness.records}
    try:
        for adapter in args.adapters:
            info = harness.cec(adapter)
            if "cec-gpio" not in info:
                # --skip-info omits driver data, so inspect explicitly.
                info = harness.command(["cec-ctl", "-d", adapter])
            if "cec-gpio" not in info:
                raise RuntimeError(f"{adapter} is not a cec-gpio fixture adapter")
            print(info)
        if args.inspect:
            report["passed"] = True
            return 0

        # Unregistered initiators do not claim either role used by the DUT.
        for adapter in args.adapters:
            harness.cec(adapter, "--clear", "--unregistered", "--phys-addr", "2.0.0.0")
        peers = {}
        for adapter in args.adapters:
            peers[adapter] = [address for address in (0, 4, 8, 11, 12, 13, 14) if harness.poll(adapter, address)]
        front = [(adapter, address) for adapter, addresses in peers.items() for address in addresses if address in (4, 8, 11, 12, 13)]
        back = [(adapter, address) for adapter, addresses in peers.items() for address in addresses if address in (0, 14)]
        if len(front) != 1 or len(back) != 1 or front[0][0] == back[0][0]:
            raise RuntimeError(f"Could not identify one Amity playback and one Amity TV: {peers}. Check boot and wiring.")
        front_adapter, front_address = front[0]
        back_adapter, back_address = back[0]
        report["mapping"] = {"front": front[0], "back": back[0]}
        harness.cec(front_adapter, "--clear", "--tv", "--phys-addr", "0.0.0.0",
                    "--osd-name", "HarnessTV", "--vendor-id", "0x000001")
        harness.cec(back_adapter, "--clear", "--playback", "--phys-addr", "1.0.0.0",
                    "--osd-name", "HarnessSource", "--vendor-id", "0x000002")
        print(f"Front: {front_adapter} -> Amity playback {front_address}; back: {back_adapter} -> Amity TV {back_address}")
        for adapter, address, physical, device_type, all_types in (
            (front_adapter, front_address, 0x1000, 4, 0x10),
            (back_adapter, back_address, 0, 0, 0x80),
        ):
            harness.expect(adapter, address, "--give-osd-name", b"\x47amity")
            harness.expect(adapter, address, "--give-physical-addr", bytes([0x84, physical >> 8, physical & 255, device_type]))
            harness.expect(adapter, address, "--get-cec-version", bytes([0x9e, 6]))
            harness.expect(adapter, address, "--give-device-vendor-id", bytes([0x87, 0, 0, 0]))
            harness.expect(adapter, address, "--give-features", bytes([0xa6, 6, all_types, 0, 0]))
            harness.expect(adapter, address, "--give-device-power-status", bytes([0x90, 1]))
            print(f"{adapter}: identity, CEC 2.0, features, vendor, and standby power passed")

        def stress(adapter, address, payload):
            for _ in range(args.iterations):
                harness.expect(adapter, address, "--give-physical-addr", payload)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            work = [pool.submit(stress, front_adapter, front_address, bytes([0x84, 0x10, 0, 4])),
                    pool.submit(stress, back_adapter, back_address, bytes([0x84, 0, 0, 0]))]
            for result in work:
                result.result()
        report["passed"] = True
        print(f"PASS: dual-bus identities and {args.iterations} concurrent request pairs")
        return 0
    except (RuntimeError, AssertionError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
