#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""SSH ProxyCommand over an already logged-in Raspberry Pi USB shell.

Use the ESP-IDF Python environment, which supplies pyserial. Only one process
may own the Pi console. This changes no network configuration and forwards
only to the Pi's localhost:22. Keep the enclosing SSH session bounded by
--timeout; EOF/signals send an explicit framed close and restore the Pi tty.
"""
import argparse
import base64
import os
from pathlib import Path
import secrets
import select
import shlex
import signal
import sys
import time
import zlib

import serial

from pi_serial_bridge import Frames, MAX_PAYLOAD, frame, write_all


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem31101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 43200:
        parser.error("--timeout must be 1..43200 seconds")
    token = secrets.token_hex(12)
    marker = ("\x1eAMITY_SERIAL_" + token + "\x1f\n").encode()
    program = Path(__file__).with_name("pi_serial_bridge.py").read_bytes()
    encoded = base64.b64encode(zlib.compress(program, 9)).decode()
    loader = "import base64,zlib;exec(zlib.decompress(base64.b64decode(" + repr(encoded) + ")))"
    command = shlex.join(["python3", "-c", loader, token, str(args.timeout)])
    if len(command) >= 4000:
        raise RuntimeError("Bootstrap exceeds the serial shell's canonical line limit")
    port = serial.Serial(None, args.baud, timeout=.1, write_timeout=5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    protocol = Frames()
    ready = False
    remote_closed = False
    stop = False

    def interrupted(_signal, _context):
        nonlocal stop
        stop = True

    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, interrupted)
    try:
        port.reset_input_buffer()
        port.write(("\n" + command + "\n").encode())
        bootstrap = bytearray()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not stop:
            bootstrap.extend(port.read(port.in_waiting or 1))
            position = bootstrap.find(marker)
            if position >= 0:
                ready = True
                port.write(frame(b"H"))
                protocol.feed(bootstrap[position + len(marker):])
                break
            if len(bootstrap) > 65536:
                raise RuntimeError("Unexpectedly large serial bootstrap output")
        if not ready:
            raise TimeoutError("Pi serial shell did not start the bridge")
        connected = False
        deadline = time.monotonic() + args.timeout + 10
        while not stop and time.monotonic() < deadline:
            readers = [port.fileno()]
            if connected:
                readers.append(0)
            readable = select.select(readers, [], [], .25)[0]
            if port.fileno() in readable:
                data = port.read(port.in_waiting or 1)
                for kind, payload in protocol.feed(data):
                    if kind == b"H" and not payload:
                        connected = True
                    elif kind == b"D":
                        write_all(1, payload)
                    elif kind == b"E":
                        print("Pi serial bridge: " + payload.decode(errors="replace"), file=sys.stderr)
                    elif kind == b"C" and not payload:
                        remote_closed = True
                        return 0
                    else:
                        raise RuntimeError("Unexpected serial bridge response")
            if connected and 0 in readable:
                data = os.read(0, MAX_PAYLOAD)
                if not data:
                    return 0
                port.write(frame(b"D", data))
        return 0
    finally:
        if ready and not remote_closed:
            try:
                port.write(frame(b"C"))
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if any(kind == b"C" for kind, _ in protocol.feed(port.read(port.in_waiting or 1))):
                        break
            except (OSError, ValueError, serial.SerialException):
                pass
        port.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, serial.SerialException) as error:
        print("Pi serial proxy: " + str(error), file=sys.stderr)
        raise SystemExit(1)
