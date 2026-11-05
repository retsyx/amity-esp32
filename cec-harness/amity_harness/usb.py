"""Read-only Pi USB inventory and bounded local chip reboot.

The runner uses only the standard library. Its local worker imports the
Espressif reset primitive; it sends no serial bytes or bootloader commands.
The caller must verify the new boot identity through the shipping UI.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys


def _usb_serial(value):
    if not isinstance(value, str) or not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value):
        raise ValueError("USB reset requires the DUT's documented MAC / USB serial")
    return value.upper()


class USBWorkerError(RuntimeError):
    """A fixed worker error code, safe to retain without arbitrary output."""


def _matching_device(serial_number, ports):
    matches = [port for port in ports
               if port.vid == 0x303A and port.pid == 0x1001
               and isinstance(port.serial_number, str) and port.serial_number.upper() == serial_number]
    if not matches:
        raise USBWorkerError("USB_DEVICE_NOT_FOUND")
    if len(matches) != 1:
        raise USBWorkerError("USB_DEVICE_AMBIGUOUS")
    device = matches[0].device
    if not isinstance(device, str) or not device or "\x00" in device:
        raise USBWorkerError("USB_DEVICE_INVALID")
    return device


def verify_usb(wiring, *, enumerate_ports=None, check_access=os.access):
    """Verify every documented DUT is uniquely connected and usable on this Pi.

    This checks USB identity and device permissions only. It never opens a
    serial port, changes modem-control lines, or imports flashing dependencies.
    """
    if enumerate_ports is None:
        from serial.tools.list_ports import comports
        enumerate_ports = comports
    ports = list(enumerate_ports())
    result = []
    for identity in wiring.identities.values():
        serial_number = _usb_serial(identity.mac)
        try:
            device = _matching_device(serial_number, ports)
            if not check_access(device, os.R_OK | os.W_OK):
                raise USBWorkerError("USB_DEVICE_NOT_ACCESSIBLE")
        except USBWorkerError as error:
            raise RuntimeError(f"USB verification failed for {identity.name}: {error}") from None
        result.append({"dut": identity.name, "usb_serial": serial_number, "port": device})
    return result


def reset_usb(serial_number, *, serial_factory=None, enumerate_ports=None, hard_reset=None):
    """Reset exactly one documented native USB device, without byte I/O."""
    serial_number = _usb_serial(serial_number)
    if serial_factory is None:
        from serial import Serial
        serial_factory = Serial
    if enumerate_ports is None:
        from serial.tools.list_ports import comports
        enumerate_ports = comports
    if hard_reset is None:
        from esp_pylib.serial_reset import hard_reset

    device = _matching_device(serial_number, enumerate_ports())

    port = serial_factory(port=None, baudrate=115200, timeout=1, write_timeout=1, exclusive=True)
    try:
        # Establish the application-boot line state before opening. Opening
        # may itself pulse reset; this operation deliberately requests reboot.
        port.dtr = False
        port.rts = False
        port.port = device
        port.open()
        # This is the same EN pulse used by esptool's native-USB hard reset.
        # Call the primitive directly so unsupported control ioctls fail.
        hard_reset(port, hold_delay=0.2, post_release_delay=0.2)
    finally:
        port.close()
    return {"usb_serial": serial_number, "port": device, "reset": True}


def _failure(message, *, stdout=None, stderr=None):
    error = RuntimeError(message)
    # Keep subprocess evidence available to an explicit private diagnostic
    # capture, without placing arbitrary worker output in the exception text.
    error.stdout, error.stderr = stdout, stderr
    return error


class USBReset:
    def __init__(self, *, python=sys.executable, timeout=30, runner=subprocess.run):
        python = os.fspath(python)
        if not isinstance(python, str) or not python or "\x00" in python:
            raise ValueError("USB Python must name an executable")
        if (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                or not math.isfinite(timeout) or timeout <= 0):
            raise ValueError("USB reset timeout must be positive and finite")
        if not callable(runner):
            raise ValueError("USB reset runner must be callable")
        self.python, self.timeout, self.runner = python, timeout, runner

    def __call__(self, dut):
        from .trace import event

        serial_number = _usb_serial(dut.identity.mac)
        command = [self.python, str(Path(__file__).resolve()), "--usb-serial", serial_number]
        event("usb.reset.request", dut=dut.name, usb_serial=serial_number)
        try:
            completed = self.runner(command, check=False, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=self.timeout, text=True)
        except subprocess.TimeoutExpired as error:
            raise _failure("USB reset worker timed out", stdout=error.stdout, stderr=error.stderr) from None
        except OSError as error:
            raise _failure("USB reset worker could not start: " + type(error).__name__) from None
        try:
            result = json.loads(completed.stdout)
        except (TypeError, ValueError):
            result = {}
        if completed.returncode != 0:
            code = result.get("error") if isinstance(result, dict) else None
            if code not in {"USB_DEVICE_NOT_FOUND", "USB_DEVICE_AMBIGUOUS", "USB_DEVICE_INVALID",
                            "DEPENDENCY_UNAVAILABLE", "USB_RESET_FAILED"}:
                code = "USB_WORKER_FAILED"
            error_type = result.get("error_type") if isinstance(result, dict) else None
            suffix = ": " + error_type if isinstance(error_type, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,80}", error_type) else ""
            raise _failure("USB reset failed: " + code + suffix,
                           stdout=completed.stdout, stderr=completed.stderr)
        if (not isinstance(result, dict) or result.get("reset") is not True
                or result.get("usb_serial") != serial_number
                or not isinstance(result.get("port"), str) or not result["port"] or "\x00" in result["port"]):
            raise _failure("USB reset worker returned an invalid confirmation",
                           stdout=completed.stdout, stderr=completed.stderr)
        event("usb.reset.submitted", dut=dut.name, usb_serial=serial_number, port=result["port"])
        return result["port"]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Verify documented USB DUTs or reset one without serial byte I/O")
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--usb-serial", type=_usb_serial)
    operation.add_argument("--check", action="store_true", help="verify documented DUT USB identities and permissions without opening ports")
    parser.add_argument("--wiring", type=Path, help="bench TEST-HARNESS.md for --check")
    args = parser.parse_args(argv)
    if args.wiring is not None and not args.check:
        parser.error("--wiring requires --check")
    try:
        if args.check:
            if __package__:
                from .wiring import Wiring
            else:
                from wiring import Wiring
            wiring = Wiring.from_markdown(args.wiring) if args.wiring else Wiring.from_markdown()
            result = {"devices": verify_usb(wiring), "verified": True}
        else:
            result = reset_usb(args.usb_serial)
    except Exception as error:
        code = (str(error) if isinstance(error, USBWorkerError) else
                "DEPENDENCY_UNAVAILABLE" if isinstance(error, ImportError) else
                "USB_VERIFICATION_FAILED" if args.check else "USB_RESET_FAILED")
        print(json.dumps({"error": code, "error_type": type(error).__name__}), flush=True)
        return 1
    print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
