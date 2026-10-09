#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Run real Matter interactions against a commissioned Amity ESP32.

Keep chip-tool fabric storage between runs to test reboot/OTA persistence.
Commissioning is separate so this test never handles WiFi credentials.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    project = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get("ESP_MATTER_PATH", Path.home() / ".espressif/v6.1/esp-matter"))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chip-tool", type=Path, default=sdk / "connectedhomeip/connectedhomeip/out/host/chip-tool")
    parser.add_argument("--node", default="0x1234")
    parser.add_argument("--storage", type=Path, default=project / "test-results/matter")
    parser.add_argument("--exercise-controls", action="store_true", help="Send playback, navigation and volume commands to the configured activity")
    parser.add_argument("--expect-unknown-audio", action="store_true", help="Assert unknown volume is null and mute is a read error")
    args = parser.parse_args()
    if not args.chip_tool.is_file():
        parser.error("chip-tool executable does not exist; supply --chip-tool")
    args.storage.mkdir(parents=True, exist_ok=True, mode=0o700)
    results = args.storage / "interactions"
    results.mkdir(exist_ok=True, mode=0o700)
    failures = []
    ansi = re.compile(r"\x1b\[[0-9;]*m")

    def run(name, command, pattern=None, expect_error=False):
        try:
            proc = subprocess.run([str(args.chip_tool), *command, "--storage-directory", str(args.storage), "--timeout", "15"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=25)
            output = ansi.sub("", proc.stdout)
            passed = (proc.returncode != 0 if expect_error else proc.returncode == 0)
            if pattern:
                passed = passed and bool(re.search(pattern, output, re.IGNORECASE))
        except subprocess.TimeoutExpired:
            output = "Interaction timed out\n"
            passed = False
        with os.fdopen(os.open(results / f"{name}.log", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as log:
            log.write(output)
        print(f"{'PASS' if passed else 'FAIL'} {name}", flush=True)
        if not passed:
            failures.append(name)

    def read(name, cluster, attribute, endpoint, pattern=None, expect_error=False):
        run(name, [cluster, "read", attribute, args.node, str(endpoint)], pattern, expect_error)

    read("player-device-type", "descriptor", "device-type-list", 1, r"DeviceType\s*[:=]\s*(40|0x0*28)\b")
    read("speaker-device-type", "descriptor", "device-type-list", 2, r"DeviceType\s*[:=]\s*(34|0x0*22)\b")
    def cluster_list_pattern(ids):
        return "(?s)" + "".join(rf"(?=.*\[\d+\]\s*:\s*{cluster}\b)" for cluster in ids)

    read("player-clusters", "descriptor", "server-list", 1, cluster_list_pattern((6, 1286, 1287, 1289)))
    read("speaker-clusters", "descriptor", "server-list", 2, cluster_list_pattern((6, 8)))
    read("activities", "mediainput", "input-list", 1)
    read("active-activity", "mediainput", "current-input", 1)
    read("player-power", "onoff", "on-off", 1)
    read("speaker-volume", "levelcontrol", "current-level", 2, r"null" if args.expect_unknown_audio else None)
    if args.expect_unknown_audio:
        read("speaker-mute-unknown", "onoff", "on-off", 2, r"Response Failure", expect_error=True)
    read("fabric-persisted", "operationalcredentials", "commissioned-fabrics", 0)
    # These validate the adapter without changing CEC devices.
    run("absolute-volume-rejected", ["levelcontrol", "move-to-level", "100", "0", "0", "0", args.node, "2"],
        r"UNSUPPORTED_COMMAND|UnsupportedCommand|0x00000581", expect_error=True)
    run("unsupported-key-rejected", ["keypadinput", "send-key", "64", args.node, "1"], r"Status\s*[:=]\s*1\b")
    run("zero-volume-step", ["levelcontrol", "step", "0", "0", "0", "0", "0", args.node, "2"])
    if args.exercise_controls:
        for command in ("play", "pause", "stop", "previous", "next"):
            run(f"playback-{command}", ["mediaplayback", command, args.node, "1"], r"Status\s*[:=]\s*0\b")
        for name, key in (("up", 1), ("down", 2), ("left", 3), ("right", 4), ("select", 0),
                          ("home", 9), ("back", 13), ("menu", 11), ("digit-zero", 32), ("digit-nine", 41)):
            run(f"key-{name}", ["keypadinput", "send-key", str(key), args.node, "1"], r"Status\s*[:=]\s*0\b")
        for mode in (0, 1):
            run(f"volume-step-{mode}", ["levelcontrol", "step", str(mode), "1", "0", "0", "0", args.node, "2"])
    print(f"Matter interactions: {len(failures)} failures; logs: {results}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
