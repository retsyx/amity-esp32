#!/usr/bin/env python3
# Copyright 2026.
# This file is part of Amity-esp32.
# Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
# GNU General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.

"""Pi-side framed USB-console bridge to localhost SSH; Python stdlib only.

Launched by pi_serial_proxy.py through an already logged-in serial shell.
Each frame is one ASCII type byte, a four-byte big-endian length, and payload.
H starts the connection, D carries bytes, C closes, E reports a bridge error.
"""
import os
import select
import signal
import socket
import struct
import sys
import termios
import time
import tty

MAX_PAYLOAD = 16384


def frame(kind, payload=b""):
    if len(kind) != 1 or len(payload) > MAX_PAYLOAD:
        raise ValueError("Invalid serial bridge frame")
    return kind + struct.pack(">I", len(payload)) + payload


class Frames:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= 5:
            size = struct.unpack(">I", self.buffer[1:5])[0]
            if size > MAX_PAYLOAD:
                raise ValueError("Serial bridge frame exceeds maximum size")
            if len(self.buffer) < 5 + size:
                break
            result.append((bytes(self.buffer[:1]), bytes(self.buffer[5:5 + size])))
            del self.buffer[:5 + size]
        return result


def write_all(fd, data):
    while data:
        sent = os.write(fd, data)
        if not sent:
            raise OSError("Serial bridge output closed")
        data = data[sent:]


def main():
    marker, duration = sys.argv[1], float(sys.argv[2])
    if not 1 <= duration <= 43200:
        raise ValueError("Bridge duration must be 1..43200 seconds")
    original = termios.tcgetattr(0)
    connection = None
    parser = Frames()

    def interrupted(_signal, _context):
        raise InterruptedError("Serial bridge interrupted")

    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, interrupted)
    try:
        tty.setraw(0, termios.TCSANOW)
        write_all(1, ("\x1eAMITY_SERIAL_" + marker + "\x1f\n").encode())
        # If bootstrap output was lost, do not strand the shell in raw mode.
        deadline = time.monotonic() + 10
        started = False
        while time.monotonic() < deadline and not started:
            if not select.select([0], [], [], min(.5, deadline - time.monotonic()))[0]:
                continue
            data = os.read(0, MAX_PAYLOAD)
            if not data:
                return
            for kind, payload in parser.feed(data):
                if kind == b"C":
                    return
                if kind != b"H" or payload:
                    raise ValueError("Expected serial bridge hello")
                started = True
        if not started:
            raise TimeoutError("Serial bridge hello timed out")
        connection = socket.create_connection(("127.0.0.1", 22), timeout=5)
        write_all(1, frame(b"H"))
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            ready = select.select([0, connection], [], [], min(.5, deadline - time.monotonic()))[0]
            if 0 in ready:
                data = os.read(0, MAX_PAYLOAD)
                if not data:
                    return
                for kind, payload in parser.feed(data):
                    if kind == b"C" and not payload:
                        return
                    if kind != b"D":
                        raise ValueError("Unexpected serial bridge frame")
                    connection.sendall(payload)
            if connection in ready:
                data = connection.recv(MAX_PAYLOAD)
                if not data:
                    return
                write_all(1, frame(b"D", data))
        raise TimeoutError("Serial bridge duration expired")
    except (OSError, ValueError) as error:
        try:
            write_all(1, frame(b"E", str(error).encode()[:1024]))
        except OSError:
            pass
    finally:
        if connection:
            connection.close()
        try:
            write_all(1, frame(b"C"))
        except OSError:
            pass
        termios.tcsetattr(0, termios.TCSANOW, original)


if __name__ == "__main__":
    main()
