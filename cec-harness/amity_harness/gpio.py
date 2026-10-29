"""Bounded physical reset-button holds locally or in one Pi-owned SSH process.

Only the GPIO resolved from TEST-HARNESS.md and the requested duration cross
SSH. No DUT HTTP credentials or control APIs are involved.
"""

import argparse
from contextlib import contextmanager
import fcntl
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import threading
import time
from uuid import uuid4


_PROTOCOL_VERSION = 1


def _pin(gpio):
    if type(gpio) is not int or not 0 <= gpio <= 27:
        raise ValueError("GPIO must be an integer on the Pi's GPIO header")
    return gpio


def _duration(seconds):
    if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or not 0 < seconds <= 60):
        raise ValueError("GPIO hold must be positive, finite and at most 60 seconds")
    if 8 < seconds < 10:
        raise ValueError("Add holds must be at most 8 seconds; factory reset requires an explicit 10-second hold")
    return float(seconds)


@contextmanager
def _signals(seconds):
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError("Physical GPIO holds need a main-thread worker for signal cleanup")
    if any(signal.getitimer(signal.ITIMER_REAL)):
        raise RuntimeError("An existing process timer prevents a bounded GPIO hold")
    previous = {}

    def stop(signum, frame):
        if signum == signal.SIGALRM:
            raise TimeoutError("Physical GPIO hold exceeded its safety deadline")
        raise InterruptedError("Physical GPIO hold interrupted by " + signal.Signals(signum).name)

    try:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM):
            previous[signum] = signal.signal(signum, stop)
        # Add is bounded independently of SSH or the calling host. Explicit
        # factory-reset holds retain their requested duration and a small bound.
        signal.setitimer(signal.ITIMER_REAL, min(seconds + 2, 8) if seconds < 10 else seconds + 2)
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def hold_low(backend, gpio, seconds):
    """Drive low, time the hold in this process, and always release the input.

    Add-range operations have an eight-second safety deadline. Durations of
    ten seconds or longer explicitly retain the existing factory-reset gesture.
    GPIO subprocess operations must themselves be bounded (PinctrlGPIO is).
    """
    gpio, seconds = _pin(gpio), _duration(seconds)
    lock_path = Path(tempfile.gettempdir()) / f"amity-reset-gpio-{gpio}.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with _signals(seconds):
            try:
                backend.low(gpio)
                time.sleep(seconds)
            finally:
                previous_mask = signal.pthread_sigmask(
                    signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM})
                try:
                    signal.setitimer(signal.ITIMER_REAL, 0)
                    backend.release(gpio)
                finally:
                    # Pending interruption is delivered only after the bounded
                    # release has finished; keep the caller's original mask.
                    signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
    finally:
        os.close(fd)
    return {"gpio": gpio, "seconds": seconds, "released": True}


class SSHPinctrlGPIO:
    """Send one complete physical hold to the Pi; never split low/release RPCs."""

    def __init__(self, *, host="pi@test2.local", root="/home/pi/amity/esp32/cec-harness",
                 port=None, runner=subprocess.run):
        if (not isinstance(host, str) or not host or host.startswith("-")
                or "\x00" in host or any(character.isspace() for character in host)):
            raise ValueError("Invalid SSH host")
        if not isinstance(root, str) or not root.startswith("/") or "\x00" in root:
            raise ValueError("The remote harness root must be an absolute path")
        if port is not None and (type(port) is not int or not 1 <= port <= 65535):
            raise ValueError("Invalid SSH port")
        self.host, self.root, self.port, self.runner = host, root, port, runner

    def hold_low(self, gpio, seconds):
        gpio, seconds = _pin(gpio), _duration(seconds)
        request_id = uuid4().hex
        remote = shlex.join(["env", f"PYTHONPATH={self.root}", "python3", "-u", "-m",
                             "amity_harness.gpio", "--gpio", str(gpio), "--seconds", str(seconds),
                             "--request-id", request_id])
        command = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                   "-o", "StrictHostKeyChecking=yes", "-o", "ServerAliveInterval=2",
                   "-o", "ServerAliveCountMax=2", *(["-p", str(self.port)] if self.port else []),
                   self.host, remote]
        completed = self.runner(command, check=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, timeout=seconds + 20)
        result = json.loads(completed.stdout)
        if (not isinstance(result, dict)
                or set(result) != {"protocol_version", "request_id", "gpio", "seconds", "released"}
                or type(result["protocol_version"]) is not int or result["protocol_version"] != _PROTOCOL_VERSION
                or result["request_id"] != request_id or _pin(result["gpio"]) != gpio
                or _duration(result["seconds"]) != seconds or result["released"] is not True):
            raise RuntimeError("The Pi did not confirm the requested GPIO hold and release")
        return {key: result[key] for key in ("gpio", "seconds", "released")}


def main(argv=None, *, backend=None):
    parser = argparse.ArgumentParser(description="Perform one bounded Pi reset GPIO hold")
    parser.add_argument("--gpio", type=int, required=True)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--request-id", required=True)
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-f]{32}", args.request_id):
        parser.error("Invalid GPIO request identity")
    gpio, seconds = _pin(args.gpio), _duration(args.seconds)
    if backend is None:
        from .dut import PinctrlGPIO
        backend = PinctrlGPIO()
    result = hold_low(backend, gpio, seconds)
    print(json.dumps({"protocol_version": _PROTOCOL_VERSION, "request_id": args.request_id, **result}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
