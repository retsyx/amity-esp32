#!/usr/bin/env python3
"""Commission the DUT with chip-tool without displaying network credentials.

The WiFi file is a private JSON object with ssid/password. The onboarding QR
payload is either its qr_payload member or the DUT's private serial boot log.
Controller keys and commissioning diagnostics are kept in --storage.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    project = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get("ESP_MATTER_PATH", Path.home() / ".espressif/v6.1/esp-matter")) / "connectedhomeip/connectedhomeip"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chip-tool", type=Path, default=sdk / "out/host/chip-tool")
    parser.add_argument("--wifi-file", type=Path, required=True)
    parser.add_argument("--boot-log", type=Path)
    parser.add_argument("--node", default="0x1234")
    parser.add_argument("--storage", type=Path, default=project / "test-results/matter")
    parser.add_argument("--paa-trust-store", type=Path, default=sdk / "credentials/development/paa-root-certs")
    parser.add_argument("--direct-ble", action="store_true", help="Use explicit BLE pairing instead of code-based parallel discovery")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if args.wifi_file.stat().st_mode & 0o077:
        parser.error("WiFi file must be private (chmod 600)")
    credentials = json.loads(args.wifi_file.read_text())
    qr = credentials.get("qr_payload")
    if args.boot_log:
        matches = re.findall(r"AMITY MATTER QR PAYLOAD:\s*(MT:[A-Z0-9.\-]+)", args.boot_log.read_text(errors="replace"))
        if matches:
            qr = matches[-1]
    if not qr or not str(qr).startswith("MT:"):
        parser.error("No Matter QR payload found in WiFi file or boot log")
    if not isinstance(credentials.get("ssid"), str) or not isinstance(credentials.get("password"), str):
        parser.error("WiFi JSON must contain string ssid and password members")
    args.storage.mkdir(parents=True, exist_ok=True, mode=0o700)
    pairing = ["code-wifi", args.node, credentials["ssid"], credentials["password"], qr]
    if args.direct_ble:
        env = dict(os.environ, TMPDIR=str(args.storage.resolve()))
        parsed = subprocess.run([str(args.chip_tool), "payload", "parse-setup-payload", qr],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, check=True)
        pin = re.search(r"Passcode:\s*(\d+)", parsed.stdout)
        discriminator = re.search(r"Long discriminator:\s*(\d+)", parsed.stdout)
        if not pin or not discriminator:
            parser.error("Could not decode onboarding payload")
        pairing = ["ble-wifi", args.node, credentials["ssid"], credentials["password"], pin[1], discriminator[1]]
    command = [str(args.chip_tool), "pairing", *pairing,
               "--storage-directory", str(args.storage),
               "--paa-trust-store-path", str(args.paa_trust_store), "--timeout", str(args.timeout)]
    print("Starting Matter BLE commissioning and WiFi provisioning", flush=True)
    output = ""
    returncode = 1
    try:
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=args.timeout + 30)
        output, returncode = proc.stdout, proc.returncode
    except subprocess.TimeoutExpired as err:
        output = (err.stdout or b"").decode(errors="replace") if isinstance(err.stdout, bytes) else (err.stdout or "")
        output += "\nCommissioning timed out\n"
    # The controller may log commissioning material. Never echo raw diagnostics.
    logfile = args.storage / "commissioning.log"
    with os.fdopen(os.open(logfile, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as log:
        log.write(output)
    print(f"Matter commissioning {'passed' if returncode == 0 else 'failed'}; private diagnostics: {logfile}", flush=True)
    return 0 if returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
