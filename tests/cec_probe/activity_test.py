#!/usr/bin/env python3
"""Physical activity checks for the isolated cec_probe image (requires pyserial)."""
import argparse
import concurrent.futures
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cec_hardware import Harness, raw_frames


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="pi@10.0.1.119")
    p.add_argument("--port", default="/dev/cu.usbmodem31201")
    p.add_argument("--output", type=Path, default=Path("test-results/cec-probe-activity.json"))
    args = p.parse_args()
    harness = Harness(args.host)
    result = {"passed": False, "commands": harness.records}
    monitors = []
    uart = serial.Serial(None, 115200, timeout=0.1)
    uart.dtr = False
    uart.rts = False
    uart.port = args.port
    uart.open()
    log = bytearray()

    def settle(seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            log.extend(uart.read(8192))

    try:
        uart.write(b"s\n")
        settle(6)
        for adapter in ("/dev/cec0", "/dev/cec1"):
            command = shlex.join(["sudo", "-n", "cec-ctl", "-d", adapter, "-s", "-r", "-v", "-M", "--monitor-time", "12"])
            process = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                                        "-o", "UserKnownHostsFile=/dev/null", args.host, command],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            monitors.append((adapter, process))
        settle(.5)
        uart.write(b"1\n")
        settle(1.5)
        harness.expect("/dev/cec0", 4, "--give-device-power-status", bytes([0x90, 0]))
        harness.expect("/dev/cec1", 0, "--give-device-power-status", bytes([0x90, 0]))
        uart.write(b"k+\n")
        settle(1.5)
        uart.write(b"0\n")
        settle(1.5)
        harness.expect("/dev/cec0", 4, "--give-device-power-status", bytes([0x90, 1]))
        harness.expect("/dev/cec1", 0, "--give-device-power-status", bytes([0x90, 1]))
        # A source activation while off must force standby on both buses.
        harness.cec("/dev/cec1", "--active-source", "phys-addr=1.0.0.0")
        settle(1)
        captures = {}
        for adapter, process in monitors:
            stdout, stderr = process.communicate(timeout=15)
            if process.returncode:
                raise RuntimeError(stderr)
            captures[adapter] = stdout
        front = raw_frames(captures["/dev/cec0"])
        back = raw_frames(captures["/dev/cec1"])
        result["captures"] = captures
        for expected in ("40:04", "40:36", "4f:36"):
            assert bytes.fromhex(expected.replace(":", "")) in front, f"Missing front {expected}"
        for expected in ("04:44:6d", "04:45", "0f:86:10:00", "04:44:00", "04:44:41", "04:36", "0f:36"):
            assert bytes.fromhex(expected.replace(":", "")) in back, f"Missing back {expected}"
        result["passed"] = True
        print("PASS: physical device discovery, activity power/routing, source/audio keys, power feedback, source-theft standby")
        return 0
    except (AssertionError, RuntimeError, subprocess.SubprocessError) as exc:
        result["error"] = str(exc)
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        for _, process in monitors:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)
        uart.close()
        result["serial"] = log.decode(errors="replace")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
