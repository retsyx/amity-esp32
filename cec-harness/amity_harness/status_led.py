"""Optional Pi running indication, owned by pipe lifetime rather than DUT state.

One unprivileged helper per run shares an advisory lock with other helpers.
Only the first changes the LEDs; the last restores their original settings.
The local helper also uses a pipe, so parent-process death closes its stdin.
"""

import argparse
from contextlib import contextmanager
import fcntl
import json
import logging
import os
from pathlib import Path
import re
import select
import shlex
import signal
import stat
import subprocess
import sys
import tempfile
import time
from uuid import uuid4


_VERSION = 1
_ROOT = "/home/pi/amity/esp32/cec-harness"
_LED_ROOT = Path("/sys/class/leds")
_NAMES = ("ACT", "PWR")
_FILES = {"trigger", "brightness", "delay_on", "delay_off"}
_START_TIMEOUT = 20
_CLOSE_TIMEOUT = 40
_LOCK_TIMEOUT = 5


class _Stopped(BaseException):
    pass


def _command(host=None, root=_ROOT, port=None):
    if (host is not None and (not isinstance(host, str) or not host or host.startswith("-")
                             or "\x00" in host or any(c.isspace() for c in host))):
        raise ValueError("Invalid SSH host")
    if not isinstance(root, str) or not root.startswith("/") or "\x00" in root:
        raise ValueError("The remote harness root must be an absolute path")
    if port is not None and (type(port) is not int or not 1 <= port <= 65535):
        raise ValueError("Invalid SSH port")
    worker = ["python3", "-u", "-m", "amity_harness.status_led", "--worker"]
    if host is None:
        return [sys.executable, *worker[1:]]
    remote = shlex.join(["env", f"PYTHONPATH={root}", *worker])
    return ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=yes", "-o", "ServerAliveInterval=2",
            "-o", "ServerAliveCountMax=2", *(["-p", str(port)] if port else []), host, remote]


def _message(stream, timeout):
    deadline, data = time.monotonic() + timeout, bytearray()
    while b"\n" not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([stream], [], [], max(0, remaining))[0]:
            raise TimeoutError("Status LED helper startup timed out")
        block = os.read(stream.fileno(), 1024)
        if not block:
            raise EOFError("Status LED helper exited before startup")
        data.extend(block)
        if len(data) > 1024:
            raise ValueError("Status LED helper response is too large")
    result = json.loads(data.split(b"\n", 1)[0])
    if (not isinstance(result, dict) or type(result.get("protocol_version")) is not int
            or result["protocol_version"] != _VERSION or type(result.get("active")) is not bool
            or set(result) != {"protocol_version", "active"}):
        raise ValueError("Invalid status LED helper response")
    return result["active"]


def _close(process):
    if process.stdin is not None:
        process.stdin.close()
        process.stdin = None
    try:
        process.communicate(timeout=_CLOSE_TIMEOUT)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5)


def _event(name, **fields):
    logging.getLogger("amity_harness").info(
        "%s %s", name, json.dumps(fields, sort_keys=True, separators=(",", ":")),
        extra={"monotonic_ns": time.monotonic_ns()})


@contextmanager
def run_status_led(host=None, root=_ROOT, port=None):
    """Blink the Pi's red LED during a run; unavailable LEDs never fail tests."""
    command = _command(host, root, port)
    process, active = None, False
    try:
        environment = None
        if host is None:
            environment = dict(os.environ)
            package_root = str(Path(__file__).resolve().parents[1])
            environment["PYTHONPATH"] = package_root + os.pathsep + environment.get("PYTHONPATH", "")
        try:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, bufsize=0, env=environment)
            active = _message(process.stdout, _START_TIMEOUT)
            _event("status_led.start", active=active, remote=host is not None)
        except Exception as error:
            _event("status_led.unavailable", error_type=type(error).__name__, remote=host is not None)
        if not active and process is not None:
            # A timed-out startup may still be changing LEDs; stop it now.
            try:
                _close(process)
                process = None
            except Exception as error:
                _event("status_led.cleanup_failed", error_type=type(error).__name__)
        yield active
    finally:
        if process is not None:
            try:
                _close(process)
                _event("status_led.finish", active=active, helper_ok=process.returncode == 0)
            except Exception as error:
                _event("status_led.cleanup_failed", error_type=type(error).__name__)


class _LEDs:
    def __init__(self, root=_LED_ROOT):
        self.root = Path(root)

    def _read(self, name, field):
        return (self.root / name / field).read_text().strip()

    def _write(self, name, field, value):
        if name not in _NAMES or field not in _FILES or not isinstance(value, str):
            raise ValueError("Invalid LED setting")
        if field == "trigger":
            if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
                raise ValueError("Invalid LED trigger")
        elif not re.fullmatch(r"[0-9]{1,10}", value):
            raise ValueError("Invalid LED value")
        path = self.root / name / field
        try:
            path.write_text(value + "\n")
        except PermissionError:
            # Only fixed hardware paths may cross the privilege boundary.
            if self.root != _LED_ROOT:
                raise
            subprocess.run(["sudo", "-n", "tee", str(path)], input=value + "\n", text=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=3)

    def snapshot(self):
        result = {}
        for name in _NAMES:
            triggers = self._read(name, "trigger")
            selected = re.findall(r"\[([A-Za-z0-9_-]+)\]", triggers)
            if len(selected) != 1 or (name == "PWR" and "timer" not in triggers.replace("[", "").replace("]", "").split()):
                raise ValueError("Required Pi LED triggers are unavailable")
            values = {"trigger": selected[0], "brightness": self._read(name, "brightness")}
            if selected[0] == "timer":
                values.update({field: self._read(name, field) for field in ("delay_on", "delay_off")})
            result[name] = values
        return _state(result)

    def running(self):
        self._write("ACT", "trigger", "none")
        self._write("ACT", "brightness", "0")
        self._write("PWR", "trigger", "timer")
        self._write("PWR", "delay_on", "300")
        self._write("PWR", "delay_off", "300")

    def restore(self, state):
        errors = []
        for name, values in _state(state).items():
            try:
                self._write(name, "trigger", "none")
                self._write(name, "brightness", values["brightness"])
                self._write(name, "trigger", values["trigger"])
                for field in ("delay_on", "delay_off"):
                    if field in values:
                        self._write(name, field, values[field])
            except Exception as error:
                errors.append(error)
        if errors:
            raise errors[0]


def _state(value):
    if not isinstance(value, dict) or set(value) != set(_NAMES):
        raise ValueError("Invalid saved LED state")
    for values in value.values():
        if (not isinstance(values, dict)
                or set(values) not in ({"trigger", "brightness"}, {"trigger", "brightness", "delay_on", "delay_off"})
                or not isinstance(values.get("trigger"), str)
                or not re.fullmatch(r"[A-Za-z0-9_-]+", values["trigger"])):
            raise ValueError("Invalid saved LED state")
        for field, setting in values.items():
            if field != "trigger" and (not isinstance(setting, str) or not re.fullmatch(r"[0-9]{1,10}", setting)):
                raise ValueError("Invalid saved LED state")
    return value


def _lock(fd, operation):
    deadline = time.monotonic() + _LOCK_TIMEOUT
    while True:
        try:
            fcntl.flock(fd, operation | fcntl.LOCK_NB)
            return
        except BlockingIOError:
            if time.monotonic() >= deadline:
                raise TimeoutError("Status LED coordination lock timed out")
            time.sleep(0.02)


class _SharedLED:
    def __init__(self, leds=None, directory=None):
        self.leds = leds if leds is not None else _LEDs()
        self.directory = Path(directory) if directory else Path(tempfile.gettempdir()) / f"amity-status-led-{os.getuid()}"
        self.directory_fd = self.coordination = self.membership = None
        self.joined = False

    def _open(self, name, flags):
        fd = os.open(name, flags | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=self.directory_fd)
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or info.st_nlink != 1 or info.st_mode & 0o077):
            os.close(fd)
            raise PermissionError("Unsafe LED coordination file")
        return fd

    def _read_state(self):
        try:
            fd = self._open("original.json", os.O_RDONLY)
        except FileNotFoundError:
            return None
        with os.fdopen(fd, "r") as source:
            data = source.read(8193)
        if len(data) > 8192:
            raise ValueError("Saved LED state is too large")
        return _state(json.loads(data))

    def _save(self, state):
        temporary = "original-" + uuid4().hex
        try:
            with os.fdopen(self._open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY), "w") as output:
                json.dump(_state(state), output)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, "original.json", src_dir_fd=self.directory_fd, dst_dir_fd=self.directory_fd)
        finally:
            try:
                os.unlink(temporary, dir_fd=self.directory_fd)
            except FileNotFoundError:
                pass

    def start(self):
        self.directory.mkdir(mode=0o700, exist_ok=True)
        self.directory_fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        info = os.fstat(self.directory_fd)
        if info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise PermissionError("Unsafe LED coordination directory")
        self.coordination = self._open("coordination.lock", os.O_CREAT | os.O_RDWR)
        self.membership = self._open("membership.lock", os.O_CREAT | os.O_RDWR)
        _lock(self.coordination, fcntl.LOCK_EX)
        try:
            try:
                fcntl.flock(self.membership, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                # All transitions are under coordination, so these are stable readers.
                _lock(self.membership, fcntl.LOCK_SH)
                self.joined = True
            else:
                self.joined = True
                state = self._read_state()
                if state is not None:
                    # Recover the first run's original settings after a killed helper.
                    self.leds.restore(state)
                state = self.leds.snapshot()
                self._save(state)
                try:
                    self.leds.running()
                except BaseException:
                    self.leds.restore(state)
                    os.unlink("original.json", dir_fd=self.directory_fd)
                    raise
                fcntl.flock(self.membership, fcntl.LOCK_SH)
        finally:
            fcntl.flock(self.coordination, fcntl.LOCK_UN)

    def close(self):
        try:
            if self.joined:
                _lock(self.coordination, fcntl.LOCK_EX)
                try:
                    fcntl.flock(self.membership, fcntl.LOCK_UN)
                    try:
                        fcntl.flock(self.membership, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        pass
                    else:
                        state = self._read_state()
                        if state is not None:
                            self.leds.restore(state)
                            os.unlink("original.json", dir_fd=self.directory_fd)
                finally:
                    fcntl.flock(self.coordination, fcntl.LOCK_UN)
        finally:
            self.joined = False
            for attribute in ("membership", "coordination", "directory_fd"):
                fd = getattr(self, attribute)
                if fd is not None:
                    os.close(fd)
                    setattr(self, attribute, None)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Own the optional Pi run LED until stdin closes")
    parser.add_argument("--worker", action="store_true", required=True)
    parser.parse_args(argv)
    shared, active, previous = _SharedLED(), False, {}

    def stop(signum, frame):
        raise _Stopped()

    try:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            previous[signum] = signal.signal(signum, stop)
        try:
            shared.start()
            active = True
        except Exception:
            pass
        print(json.dumps({"protocol_version": _VERSION, "active": active}), flush=True)
        if active:
            # No commands are accepted: pipe lifetime is the entire protocol.
            while sys.stdin.buffer.read(1):
                pass
    except _Stopped:
        pass
    finally:
        mask = signal.pthread_sigmask(signal.SIG_BLOCK, set(previous))
        try:
            shared.close()
        finally:
            for signum, handler in previous.items():
                signal.signal(signum, handler)
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
