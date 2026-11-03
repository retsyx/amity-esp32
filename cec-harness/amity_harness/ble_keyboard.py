"""A real BLE keyboard fixture paired through Amity's existing remote UI.

Only keyboard reports and fixture control cross SSH. DUT authentication and
reviewed pair/unpair operations stay in the functional-test process.
"""

from copy import deepcopy
import json
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import tempfile
import threading
import time
from uuid import uuid4

from .activity import ActivityAction
from .dut import AmityHTTPError, _positive_seconds
from .trace import event


class KeyboardConflict(RuntimeError):
    """Pairing changed or an existing remote would need to be replaced."""


def _identifier(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{32}", value) is not None


def _generation(value):
    return isinstance(value, str) and value.isascii() and value.isdecimal()


def _reports(keyboard, consumer):
    keyboard = tuple(keyboard)
    if (len(keyboard) > 6 or len(set(keyboard)) != len(keyboard)
            or any(type(value) is not int or not 1 <= value <= 255 for value in keyboard)
            or type(consumer) is not int or not 0 <= consumer <= 65535):
        raise ValueError("Use at most six distinct keyboard usages and one 16-bit consumer usage")
    return sorted(keyboard), consumer


class LocalKeyboard:
    """Bounded JSON IPC to the Pi's short-lived D-Bus keyboard worker."""

    def __init__(self, *, python="python3", timeout=20):
        if not isinstance(python, str) or not python or "\x00" in python:
            raise ValueError("Choose the keyboard worker's Python executable")
        _positive_seconds(timeout, "Keyboard IPC timeout")
        self.python, self.timeout = python, timeout
        self._process = self._stderr = None
        self._sequence = 0
        self._buffer = bytearray()
        self._lock = threading.Lock()

    def _command(self):
        return [self.python, "-u", "-m", "amity_harness.ble_keyboard_worker"]

    def _environment(self):
        return {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])}

    @staticmethod
    def _remaining(deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("BLE keyboard worker deadline expired")
        return remaining

    def _rpc(self, operation, arguments=None):
        deadline = time.monotonic() + self.timeout
        if not self._lock.acquire(timeout=self._remaining(deadline)):
            raise TimeoutError("BLE keyboard worker is busy")
        try:
            if self._process is None:
                raise RuntimeError("BLE keyboard worker is not open")
            self._sequence += 1
            request = {"id": self._sequence, "operation": operation, "arguments": arguments or {}}
            data = memoryview((json.dumps(request, separators=(",", ":")) + "\n").encode())
            while data:
                _, ready, _ = select.select([], [self._process.stdin], [], self._remaining(deadline))
                if ready:
                    data = data[os.write(self._process.stdin.fileno(), data):]
            while b"\n" not in self._buffer:
                ready, _, _ = select.select([self._process.stdout], [], [], self._remaining(deadline))
                if ready:
                    chunk = os.read(self._process.stdout.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError("BLE keyboard worker ended unexpectedly")
                    self._buffer.extend(chunk)
                    if len(self._buffer) > 65536:
                        raise RuntimeError("BLE keyboard worker exceeded its reply bound")
            line, _, rest = self._buffer.partition(b"\n")
            self._buffer = bytearray(rest)
            reply = json.loads(line)
            if (not isinstance(reply, dict) or reply.get("id") != self._sequence
                    or type(reply.get("ok")) is not bool):
                raise RuntimeError("BLE keyboard worker returned an invalid reply identity")
            if reply["ok"] is not True:
                error = reply.get("error")
                detail = " (" + error + ")" if isinstance(error, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]{0,127}", error) else ""
                raise RuntimeError("BLE keyboard worker rejected " + operation + detail)
            if not isinstance(reply.get("result"), dict):
                raise RuntimeError("BLE keyboard worker returned an invalid result")
            return reply["result"]
        finally:
            self._lock.release()

    def open(self):
        if self._process is not None:
            raise RuntimeError("The BLE keyboard worker is already open")
        self._stderr = tempfile.TemporaryFile()
        try:
            self._process = subprocess.Popen(self._command(), stdin=subprocess.PIPE,
                                             stdout=subprocess.PIPE, stderr=self._stderr,
                                             env=self._environment(), bufsize=0)
            os.set_blocking(self._process.stdin.fileno(), False)
            os.set_blocking(self._process.stdout.fileno(), False)
            result = self._rpc("open", {"protocol_version": 1})
            if (result.get("protocol_version") != 1 or result.get("type") != 0
                    or not isinstance(result.get("name"), str) or not result["name"]
                    or not isinstance(result.get("address"), str)
                    or re.fullmatch(r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}", result["address"]) is None):
                raise RuntimeError("BLE keyboard worker has no valid advertised identity")
            return result
        except BaseException:
            self._shutdown()
            raise

    def status(self):
        return self._rpc("status")

    def report(self, *, keyboard=(), consumer=0):
        keyboard, consumer = _reports(keyboard, consumer)
        return self._rpc("report", {"keyboard": keyboard, "consumer": consumer})

    def disconnect(self):
        return self._rpc("disconnect")

    def _shutdown(self):
        process, self._process = self._process, None
        if process is not None:
            for stream in (process.stdin, process.stdout):
                if stream is not None:
                    stream.close()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None
        self._buffer.clear()

    def close(self):
        if self._process is None:
            return
        try:
            self._rpc("close")
        finally:
            self._shutdown()


class SSHKeyboard(LocalKeyboard):
    def __init__(self, *, host, root, port=None, python="python3", timeout=20):
        super().__init__(python=python, timeout=timeout)
        if (not isinstance(host, str) or not host or host.startswith("-")
                or any(character.isspace() for character in host) or "\x00" in host):
            raise ValueError("Invalid keyboard SSH host")
        if not isinstance(root, str) or not root.startswith("/") or "\x00" in root:
            raise ValueError("The keyboard harness root must be absolute")
        if port is not None and (type(port) is not int or not 1 <= port <= 65535):
            raise ValueError("Invalid keyboard SSH port")
        self.host, self.root, self.port = host, root, port

    def _command(self):
        remote = shlex.join(["env", "PYTHONPATH=" + self.root, self.python, "-u", "-m",
                             "amity_harness.ble_keyboard_worker"])
        return ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                "-o", "StrictHostKeyChecking=yes", *(["-p", str(self.port)] if self.port else []),
                self.host, remote]


class PairedKeyboard:
    """Own a new test keyboard bond, never replacing an existing remote."""

    _cleanup_priority = 1
    _cleanup_retryable = True

    def __init__(self, dut, *, timeout=None):
        bed = getattr(dut, "_testbed", None)
        if (bed is None or not bed._entered or bed._closed or bed.session is None
                or not any(owner is dut for owner in bed.duts)
                or any(owner is dut for owner in (*bed.members, *bed.control_only))):
            raise RuntimeError("Pair the keyboard on an original main inside its active TestBed")
        self.dut, self.bed, self.session = dut, bed, bed.session
        self.timeout = dut._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "BLE keyboard pairing timeout")
        self.worker = None
        self.boot = self.group = self.identity = None
        self._created = self._closed = self._started = self._disconnected = False
        self._pair_pending = self._unpair_pending = None
        self._pair_generation = None
        self._record = {"kind": "ble_keyboard", "device_id": dut.identity.device_id, "checks": []}
        if not hasattr(bed, "control_observations"):
            bed.control_observations = []
        bed.control_observations.append(self._record)
        bed.add_cleanup(self.close)

    def _note(self, phase, **values):
        record = {"phase": phase, **values}
        self._record["checks"].append(record)
        del self._record["checks"][:-256]
        event("ble_keyboard." + phase, dut=self.dut.name, **values)
        return record

    @staticmethod
    def _remaining(deadline):
        return LocalKeyboard._remaining(deadline)

    def _pause(self, deadline):
        time.sleep(min(.1, self._remaining(deadline)))

    def _request(self, path, data=None, *, deadline):
        return self.dut.request(path, data, timeout=min(15, self._remaining(deadline)))

    def _system(self):
        if (self.dut._testbed is not self.bed or self.bed._closed or not self.bed._entered
                or self.bed.session is not self.session):
            raise KeyboardConflict("The keyboard TestBed or CEC session changed")
        self.session.assert_healthy()
        system = self.dut.get_system()
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.dut.identity.device_id
                or system.get("mode") != "coordinator" or membership.get("coordinator") is not True
                or membership.get("authority_ready") is not True or membership.get("member") is not False
                or not _identifier(system.get("boot_id")) or not _identifier(system.get("consortium_id"))):
            raise KeyboardConflict("The keyboard requires the proven original main's authority")
        if self.boot is not None and (system["boot_id"] != self.boot or system["consortium_id"] != self.group):
            raise KeyboardConflict("The original main restarted or changed identity")
        return system

    def _status(self, deadline):
        self._system()
        state = self._request("/api/remotes", deadline=deadline)
        flags = ("ready", "bonded", "connected", "connecting", "pairing", "unpairing", "scanning")
        if (not isinstance(state, dict) or state.get("boot_id") != self.boot
                or not _generation(state.get("remote_generation"))
                or any(type(state.get(name)) is not bool for name in flags)):
            raise KeyboardConflict("The remote UI returned invalid boot or pairing state")
        return state

    def _post(self, operation, state, deadline):
        request_id = uuid4().hex
        payload = {"request_id": request_id, "expected_boot_id": self.boot,
                   "expected_remote_generation": state["remote_generation"]}
        if operation == "pair":
            payload.update(address=self.identity["address"], type=self.identity["type"])
        pending = {"request_id": request_id, "generation": str(int(state["remote_generation"]) + 1)}
        if operation == "pair":
            self._pair_pending = pending
        else:
            self._unpair_pending = pending
        self._note(operation + "_submit", request_id=request_id,
                   remote_generation=state["remote_generation"], main_boot=self.boot)
        try:
            response = self._request("/api/remotes/" + operation, payload, deadline=deadline)
        except AmityHTTPError as error:
            if error.status < 500:
                if operation == "pair":
                    self._pair_pending = None
                else:
                    self._unpair_pending = None
            raise
        if (not isinstance(response, dict) or response.get("saved") is not True
                or response.get("admitted") is not True or response.get("remote_generation") != pending["generation"]):
            raise KeyboardConflict("The remote operation was not confirmed admitted on its reviewed generation")
        pending["admitted"] = True
        return pending

    def _ready(self, status):
        if self.worker is None:
            raise KeyboardConflict("The keyboard worker ended before ownership could be verified")
        worker = self.worker.status()
        if any(type(worker.get(name)) is not bool for name in ("ready", "connected", "bonded")):
            raise KeyboardConflict("The keyboard worker returned invalid connection proof")
        return status["bonded"] and status["connected"] and worker["ready"] and worker["connected"] and worker["bonded"]

    def _finish_pair(self, deadline):
        pending = self._pair_pending
        while True:
            state = self._status(deadline)
            if state["remote_generation"] != pending["generation"]:
                raise KeyboardConflict("Remote administration changed while pairing the keyboard")
            if self._ready(state):
                self._created = True
                self._pair_generation = pending["generation"]
                self._pair_pending = None
                self._note("paired", main_boot=self.boot, remote_generation=self._pair_generation,
                           address=self.identity["address"], connected=True, bonded=True)
                return
            self._pause(deadline)

    def create(self):
        if self._started:
            raise RuntimeError("The BLE keyboard context cannot be reused")
        self._started = True
        deadline = time.monotonic() + self.timeout
        system = self._system()
        self.boot, self.group = system["boot_id"], system["consortium_id"]
        self._record["main_boot"] = self.boot
        state = self._status(deadline)
        if state["bonded"] or state["connected"]:
            raise KeyboardConflict("An existing remote is paired. This test never replaces its bond")
        if not state["ready"] or any(state[name] for name in ("pairing", "unpairing", "connecting", "scanning")):
            raise KeyboardConflict("Remote administration must be idle before pairing the test keyboard")
        factory = self.dut._runtime.keyboard_factory or LocalKeyboard
        self.worker = factory()
        self.identity = self.worker.open()
        self._request("/api/remotes/scan", {}, deadline=deadline)
        while True:
            state = self._status(deadline)
            if state["bonded"] or state["connected"]:
                raise KeyboardConflict("Another remote was paired during keyboard discovery")
            matches = [row for row in state.get("devices", []) if isinstance(row, dict)
                       and row.get("address", "").lower() == self.identity["address"].lower()
                       and row.get("type") == self.identity["type"]]
            if len(matches) == 1:
                break
            self._pause(deadline)
        self._post("pair", state, deadline)
        self._finish_pair(deadline)
        return self

    def assert_paired(self):
        if not self._created or self._closed or self._disconnected:
            raise RuntimeError("This test has no connected keyboard to verify")
        state = self._status(time.monotonic() + self.timeout)
        if state["remote_generation"] != self._pair_generation or not self._ready(state):
            raise KeyboardConflict("The test keyboard is no longer the connected bonded remote")
        return self

    def assert_disconnected(self):
        """Prove link loss while retaining this test's DUT bond for cleanup."""
        if not self._created or self._closed or not self._disconnected:
            raise RuntimeError("Disconnect this test keyboard before checking link loss")
        deadline = time.monotonic() + self.timeout
        while True:
            state = self._status(deadline)
            worker = self.worker.status()
            if state["remote_generation"] != self._pair_generation or state["bonded"] is not True:
                raise KeyboardConflict("The keyboard bond changed during its disconnect proof")
            if (state["connected"] is False and worker.get("connected") is False
                    and worker.get("ready") is False):
                self._note("disconnected", main_boot=self.boot, bonded=True, connected=False)
                return self
            self._pause(deadline)

    def checkpoint(self, *, kind="ble_report"):
        if not self._created or self._closed:
            raise RuntimeError("Pair this keyboard before sending input")
        self._system()
        scan = self.dut.last_scan
        if scan is None or scan.dut is not self.dut or scan._session is not self.session:
            raise RuntimeError("BLE keyboard input requires the current TestBed's HDMI scan")
        scan.validate_owners(scan.expected)
        state = self.dut.get_state()
        if state.get("expected_boot_id") != self.boot:
            raise KeyboardConflict("The original main restarted before keyboard input")
        return ActivityAction(self.dut, self.session, time.monotonic_ns(), self.boot,
                              deepcopy(scan.initiators), kind, uuid4().hex, None,
                              state=state, scan=scan, testbed=self.bed,
                              owner_boots=dict(scan.owner_boots), owner_duts=dict(scan.owner_duts))

    def report(self, *, keyboard=(), consumer=0):
        keyboard, consumer = _reports(keyboard, consumer)
        if self._disconnected:
            raise RuntimeError("The disconnected keyboard cannot send input")
        action = self.checkpoint()
        action.operation = self.worker.report(keyboard=keyboard, consumer=consumer)
        receipt = action.operation
        if (type(receipt.get("timestamp_monotonic_ns")) is not int or receipt["timestamp_monotonic_ns"] <= 0
                or not isinstance(receipt.get("keyboard_report"), str)
                or re.fullmatch(r"[0-9a-f]{16}", receipt["keyboard_report"]) is None
                or not isinstance(receipt.get("consumer_report"), str)
                or re.fullmatch(r"[0-9a-f]{4}", receipt["consumer_report"]) is None):
            raise KeyboardConflict("The keyboard report has no valid physical notification receipt")
        self._note("report", started_monotonic_ns=action.started_ns, keyboard=keyboard,
                   consumer=consumer, **action.operation)
        return action

    def all_up(self):
        return self.report()

    def disconnect(self):
        if self._disconnected:
            raise RuntimeError("The keyboard is already disconnected")
        action = self.checkpoint(kind="ble_disconnect")
        action.operation = self.worker.disconnect()
        self._disconnected = True
        self._note("disconnect", started_monotonic_ns=action.started_ns, **action.operation)
        return action

    def close(self):
        if self._closed:
            return
        deadline = time.monotonic() + self.timeout
        error = None
        try:
            if self._pair_pending is not None:
                # A lost POST reply is never resent. Only a fresh bonded link
                # to this worker can establish ownership for subsequent cleanup.
                self._finish_pair(deadline)
            if self._created:
                state = self._status(deadline)
                if state["bonded"] or state["connected"] or state["pairing"] or state["unpairing"]:
                    expected = (self._unpair_pending or {}).get("generation", self._pair_generation)
                    if state["remote_generation"] != expected:
                        raise KeyboardConflict("Remote administration changed before keyboard cleanup")
                    if self._unpair_pending is None:
                        if not self._disconnected and self.worker.status().get("connected") is True:
                            self.worker.report(keyboard=(), consumer=0)
                        self._post("unpair", state, deadline)
                    while True:
                        state = self._status(deadline)
                        if state["remote_generation"] != self._unpair_pending["generation"]:
                            raise KeyboardConflict("Remote administration changed during keyboard cleanup")
                        if not any(state[name] for name in ("bonded", "connected", "pairing", "unpairing", "connecting")):
                            self._unpair_pending = None
                            break
                        self._pause(deadline)
                self._created = False
                self._note("unpaired", main_boot=self.boot, bonded=False, connected=False)
        except BaseException as failure:
            error = failure
        finally:
            if self.worker is not None:
                try:
                    self.worker.close()
                except BaseException as failure:
                    if error is None:
                        error = failure
                self.worker = None
                self._disconnected = True
        if error is not None:
            raise error
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception as cleanup_error:
            if exc is None:
                raise
            event("ble_keyboard.cleanup_failed", dut=self.dut.name,
                  error_type=type(cleanup_error).__name__, primary_error_type=type(exc).__name__)
            if hasattr(exc, "add_note"):
                exc.add_note("Keyboard cleanup failed; the registered TestBed cleanup will retry it")
        return False
