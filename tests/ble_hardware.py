#!/usr/bin/env python3
"""Exercise the real HOGP client through the HTTPS API and Pi keyboard fixture.

Run only after coordinating exclusive fixture use. The default performs scan
and bonding; optional key tests require eight existing fixture activities.
--reboot verifies NVS bond restoration; --unpair leaves the DUT without a
remote and checks that Matter fabrics remain unchanged. No serial access.
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

from cec_web_activity import Admin, read_password, write_private
from cec_hardware import raw_frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--boot-log", action="append", type=Path, required=True)
    parser.add_argument("--host", default="pi@10.0.1.119")
    parser.add_argument("--ssh-port", type=int, default=22)
    parser.add_argument("--address", default="2c:cf:67:4d:4f:a9")
    parser.add_argument("--key-tests", action="store_true")
    parser.add_argument("--reboot", action="store_true")
    parser.add_argument("--unpair", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("test-results/ble-hardware.json"))
    args = parser.parse_args()
    admin = Admin(args.url)
    report = {"passed": False, "checks": [], "http": admin.records}
    pairing_disabled = False
    password = read_password(args.boot_log)

    def check(name, condition):
        if not condition:
            raise AssertionError(name)
        report["checks"].append(name)
        print("PASS", name, flush=True)

    def remote_wait(predicate, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = admin.request("/api/remotes")
            if predicate(state):
                return state
            time.sleep(.3)
        raise AssertionError("BLE state timed out: " + json.dumps(state))

    def emulator(*options):
        command = shlex.join(["sudo", "-n", "env",
            "PYTHONPATH=/home/pi/amity/esp32/tests/dbus_next-0.2.3-py3-none-any.whl",
            "/usr/bin/python3", "/home/pi/amity/esp32/tests/ble_hid_emulator.py", *options])
        result = subprocess.run(["ssh", "-p", str(args.ssh_port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                                 "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                                 args.host, command], text=True, capture_output=True, timeout=40)
        if result.returncode:
            raise RuntimeError("Pi keyboard command failed: " + result.stderr)
        return json.loads(result.stdout)

    def bonds():
        result = subprocess.run(["ssh", "-p", str(args.ssh_port), "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            args.host, "sudo -n bluetoothctl devices Bonded"],
            check=True, capture_output=True, text=True, timeout=15)
        return sorted(line.split()[1] for line in result.stdout.splitlines() if line.startswith('Device '))

    def pairable(enabled):
        subprocess.run(["ssh", "-p", str(args.ssh_port), "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            args.host, 'sudo -n bluetoothctl pairable ' + ('on' if enabled else 'off')],
            check=True, capture_output=True, text=True, timeout=15)

    try:
        admin.login(password)
        fabrics = admin.request("/api/matter")["fabrics"]
        state = admin.request("/api/remotes")
        check("shared BLE host ready", state["ready"])
        if not state["bonded"]:
            admin.request("/api/remotes/scan", {})
            state = remote_wait(lambda s: any(d["address"].lower() == args.address.lower() for d in s["devices"]), 25)
            peer = next(d for d in state["devices"] if d["address"].lower() == args.address.lower())
            report["scan_peer"] = peer
            check("HID advertisement discovered", True)
            admin.request("/api/remotes/pair", {"address": peer["address"], "type": peer["type"]})
        state = remote_wait(lambda s: s["connected"] and s["bonded"])
        report["paired_state"] = state
        check("encrypted bonded HID connection", True)
        check("battery service read", state["battery"] == 87)
        fixture = emulator("--status")
        check("keyboard and consumer notifications enabled", fixture["keyboard_subscribed"] and fixture["consumer_subscribed"])
        bonded_identities = bonds()
        report['peer_bond_identities'] = bonded_identities
        if args.key_tests:
            check("eight test activities configured", len(admin.request("/api/config")["activities"]) >= 8)
            for long_press in (False, True):
                for button in range(4):
                    emulator("--send-key", hex(0x3a + button), "--hold-ms", "650" if long_press else "150")
                    expected = button + (4 if long_press else 0)
                    admin.wait_status(lambda s: s["current_activity"] == expected)
                    check(f"F{button + 1} {'long' if long_press else 'short'} selects activity {expected}", True)
            monitor_command = shlex.join(["sudo", "-n", "cec-ctl", "-d", "/dev/cec1",
                "-s", "-r", "-v", "-M", "--monitor-time", "15"])
            monitor = subprocess.Popen(["ssh", "-p", str(args.ssh_port), "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                args.host, monitor_command], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                time.sleep(.7)
                emulator("--send-key", "0x52", "--hold-ms", "1500")
                emulator("--send-key", "0x80", "--hold-ms", "1500")
                emulator("--send-consumer", "0xe9", "--hold-ms", "150")
                stdout, stderr = monitor.communicate(timeout=20)
            finally:
                if monitor.poll() is None:
                    monitor.terminate()
                    monitor.communicate(timeout=5)
            check("CEC hold capture succeeds", monitor.returncode == 0 and "failed" not in stderr.lower())
            report["hold_capture"] = stdout
            frames = raw_frames(stdout)
            source = admin.request('/api/status')['back_address'] << 4 | 4
            check("arrow hold repeats on the physical CEC bus", frames.count(bytes([source, 0x44, 1])) >= 3)
            check("keyboard and consumer volume reach the CEC bus", frames.count(bytes([source, 0x44, 0x41])) >= 4)
            check("all three held keys release on the CEC bus", frames.count(bytes([source, 0x45])) >= 3)
            emulator("--send-key", "0x66", "--hold-ms", "650")
            admin.wait_status(lambda s: s["current_activity"] == 7)
            check("long power preserves active activity", True)
            emulator("--send-key", "0x66", "--hold-ms", "150")
            admin.wait_status(lambda s: s["current_activity"] == -1)
            check("short power enters standby", True)
            emulator("--send-key", "0x66", "--hold-ms", "150")
            admin.wait_status(lambda s: s["current_activity"] == 0)
            check("short power wakes activity zero", True)
        if args.reboot:
            # The emulator normally accepts new Just Works pairings. Disable
            # that during reconnect so lost keys cannot masquerade as a bond.
            pairable(False)
            pairing_disabled = True
            admin.request("/api/reboot", {})
            time.sleep(4)
            deadline = time.monotonic() + 60
            while True:
                try:
                    admin.login(password)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(2)
            state = remote_wait(lambda s: s["connected"] and s["bonded"])
            check("bond reconnects after reboot", state["battery"] == 87)
            check("peer bond identity unchanged after reboot", bonds() == bonded_identities)
            check("reconnect succeeds with new pairing disabled", True)
            check("Matter fabric survives reboot", admin.request("/api/matter")["fabrics"] == fabrics)
        if args.unpair:
            admin.request("/api/remotes/unpair", {})
            remote_wait(lambda s: not s["bonded"] and not s["connected"])
            check("unpair disconnects and clears selected bond", True)
            check("unpair preserves Matter fabric", admin.request("/api/matter")["fabrics"] == fabrics)
        report["final_state"] = admin.request("/api/remotes")
        report["passed"] = True
        return 0
    except Exception as error:
        report["error"] = type(error).__name__ + ": " + str(error)
        print("FAIL", report["error"], file=sys.stderr)
        return 1
    finally:
        if pairing_disabled:
            pairable(True)  # Restore the running emulator's pairing mode.
        if admin.token:
            try:
                admin.request("/api/logout", {})
            except Exception:
                report["logout_failed"] = True
        write_private(args.output, report)


if __name__ == "__main__":
    raise SystemExit(main())
