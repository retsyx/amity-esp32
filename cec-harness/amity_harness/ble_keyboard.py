"""A real BLE keyboard fixture paired through Amity's existing remote UI.

The Pi owns keyboard reports, fixture control, authentication and pair/unpair
operations through the shipping UI.
"""

from copy import deepcopy
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
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


def _keyboard_slots(value):
    if type(value) is not int or value not in (6, 10):
        raise ValueError("Choose six or ten keyboard slots")
    return value


def _reports(keyboard, consumer, keyboard_slots=6):
    _keyboard_slots(keyboard_slots)
    keyboard = tuple(keyboard)
    if (any(type(value) is not int or not 4 <= value <= 0xe7 for value in keyboard)
            or len(set(keyboard)) != len(keyboard)
            or sum(value < 0xe0 for value in keyboard) > keyboard_slots
            or type(consumer) is not int or not 0 <= consumer <= 65535):
        raise ValueError("Use at most " + str(keyboard_slots) + " ordinary keyboard usages, eight modifiers and one 16-bit consumer usage")
    return sorted(keyboard), consumer


class LocalKeyboard:
    """Bounded JSON IPC to the Pi's short-lived D-Bus keyboard worker."""

    def __init__(self, *, python=sys.executable, timeout=20, keyboard_slots=6):
        if not isinstance(python, str) or not python or "\x00" in python:
            raise ValueError("Choose the keyboard worker's Python executable")
        _positive_seconds(timeout, "Keyboard IPC timeout")
        self.python, self.timeout = python, timeout
        self.keyboard_slots = _keyboard_slots(keyboard_slots)
        self._process = self._stderr = None
        self._sequence = 0
        self._buffer = bytearray()
        self._lock = threading.Lock()

    def _command(self):
        return [self.python, "-u", "-m", "amity_harness.ble_keyboard_worker",
                "--keyboard-slots", str(self.keyboard_slots)]

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
                    or re.fullmatch(r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}", result["address"]) is None
                    or result.get("keyboard_slots", 6) != self.keyboard_slots):
                raise RuntimeError("BLE keyboard worker has no valid advertised identity")
            return result
        except BaseException:
            self._shutdown()
            raise

    def status(self):
        return self._rpc("status")

    def report(self, *, keyboard=(), consumer=0):
        keyboard, consumer = _reports(keyboard, consumer, self.keyboard_slots)
        return self._rpc("report", {"keyboard": keyboard, "consumer": consumer})

    def disconnect(self):
        return self._rpc("disconnect")

    def forget_peer(self):
        return self._rpc("forget_peer")

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


class PairedKeyboard:
    """Own a new test keyboard bond, never replacing an existing remote."""

    _cleanup_priority = 1
    _cleanup_retryable = True

    def __init__(self, dut, *, timeout=None, keyboard_slots=6):
        bed = getattr(dut, "_testbed", None)
        if (bed is None or not bed._entered or bed._closed or bed.session is None
                or not any(owner is dut for owner in bed.duts)
                or any(owner is dut for owner in (*bed.members, *bed.control_only))):
            raise RuntimeError("Pair the keyboard on an original main inside its active TestBed")
        self.dut, self.bed, self.session = dut, bed, bed.session
        self.keyboard_slots = _keyboard_slots(keyboard_slots)
        self.timeout = dut._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "BLE keyboard pairing timeout")
        self.worker = None
        self.boot = self.group = self.identity = None
        self._created = self._closed = self._started = self._disconnected = False
        self._pair_pending = self._unpair_pending = None
        self._pair_generation = None
        self._scan_state = None
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

    def _system(self, *, check_health=True, _system=None):
        if (self.dut._testbed is not self.bed or self.bed._closed or not self.bed._entered
                or self.bed.session is not self.session):
            raise KeyboardConflict("The keyboard TestBed or CEC session changed")
        if check_health:
            self.session.assert_healthy()
        system = self.dut.get_system() if _system is None else _system
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.dut.identity.device_id
                or system.get("mode") != "coordinator" or membership.get("coordinator") is not True
                or membership.get("authority_ready") is not True or membership.get("member") is not False
                or not _identifier(system.get("boot_id")) or not _identifier(system.get("consortium_id"))):
            raise KeyboardConflict("The keyboard requires the proven original main's authority")
        if self.boot is not None and (system["boot_id"] != self.boot or system["consortium_id"] != self.group):
            raise KeyboardConflict("The original main restarted or changed identity")
        return system

    def _status(self, deadline, *, check_health=True, _system=None):
        self._system(check_health=check_health, _system=_system)
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

    def _paired_link(self, status, *, require_ready=True):
        if self.worker is None:
            raise KeyboardConflict("The keyboard worker ended before ownership could be verified")
        worker = self.worker.status()
        if any(type(worker.get(name)) is not bool for name in ("ready", "connected", "bonded", "peer_owned")):
            raise KeyboardConflict("The keyboard worker returned invalid connection proof")
        connected = status["bonded"] and status["connected"] and worker["connected"] and worker["bonded"]
        if connected and not worker["peer_owned"]:
            raise KeyboardConflict("The Pi Bluetooth bond is not fixture-owned")
        return connected and (worker["ready"] or not require_ready)

    def _ready(self, status):
        return self._paired_link(status)

    def _finish_pair(self, deadline, *, check_health=True, for_cleanup=False):
        pending = self._pair_pending
        while True:
            state = self._status(deadline, check_health=check_health)
            if state["remote_generation"] != pending["generation"]:
                raise KeyboardConflict("Remote administration changed while pairing the keyboard")
            error = state.get("last_error")
            if (type(error) is int and error != 0
                    and not any(state[name] for name in ("bonded", "connected", "connecting", "pairing", "unpairing"))):
                self._pair_pending = None
                self._note("pair_failed", main_boot=self.boot, remote_generation=pending["generation"],
                           last_error=error, bonded=False, connected=False)
                raise KeyboardConflict("The DUT rejected keyboard connection or discovery (error " + str(error) + ")")
            if self._paired_link(state, require_ready=not for_cleanup):
                self._created = True
                self._pair_generation = pending["generation"]
                self._pair_pending = None
                self._note("pair_owned_for_cleanup" if for_cleanup else "paired",
                           main_boot=self.boot, remote_generation=self._pair_generation,
                           address=self.identity["address"], connected=True, bonded=True)
                return
            self._pause(deadline)

    def open(self, *, _deadline=None):
        """Register one owned keyboard advertisement without scanning or pairing."""
        if self._started or self._closed:
            raise RuntimeError("The BLE keyboard context cannot be reused")
        self._started = True
        deadline = time.monotonic() + self.timeout if _deadline is None else _deadline
        system = self._system()
        self.boot, self.group = system["boot_id"], system["consortium_id"]
        self._record["main_boot"] = self.boot
        state = self._status(deadline)
        if state["bonded"] or state["connected"]:
            raise KeyboardConflict("An existing remote is paired. This test never replaces its bond")
        if not state["ready"] or any(state[name] for name in ("pairing", "unpairing", "connecting", "scanning")):
            raise KeyboardConflict("Remote administration must be idle before pairing the test keyboard")
        factory = self.dut._runtime.keyboard_factory or LocalKeyboard
        self.worker = factory() if self.keyboard_slots == 6 else factory(keyboard_slots=self.keyboard_slots)
        self.identity = self.worker.open()
        if self.identity.get("keyboard_slots", 6) != self.keyboard_slots:
            raise KeyboardConflict("The keyboard worker did not confirm its report capacity")
        return self

    def _advertised(self):
        if not self._started or self._closed or self.worker is None:
            raise RuntimeError("Open the BLE keyboard before scanning or pairing")
        status = self.worker.status()
        if status.get("advertisement_registered") is not True:
            raise KeyboardConflict("The owned keyboard advertisement is no longer registered")
        return status

    def scan(self, *, _deadline=None):
        """Run one fresh UI scan and require this exact advertised keyboard."""
        worker = self._advertised()
        if worker.get("connected") is not False or worker.get("bonded") is not False:
            raise KeyboardConflict("The advertised keyboard already has a Bluetooth peer")
        deadline = time.monotonic() + self.timeout if _deadline is None else _deadline
        state = self._status(deadline)
        if (self._created or self._pair_pending is not None or self._unpair_pending is not None
                or not state["ready"] or any(state[name] for name in
                    ("bonded", "connected", "pairing", "unpairing", "connecting", "scanning"))):
            raise KeyboardConflict("Remote administration must be idle and unbonded before scanning")
        self._scan_state = None
        self._request("/api/remotes/scan", {}, deadline=deadline)
        while True:
            state = self._status(deadline)
            if state["bonded"] or state["connected"]:
                raise KeyboardConflict("Another remote was paired during keyboard discovery")
            if not state["scanning"]:
                matches = [row for row in state.get("devices", []) if isinstance(row, dict)
                           and isinstance(row.get("address"), str)
                           and row["address"].lower() == self.identity["address"].lower()
                           and row.get("type") == self.identity["type"]]
                if len(matches) != 1:
                    raise KeyboardConflict("The completed remote scan did not find this exact test keyboard")
                break
            self._pause(deadline)
        self._advertised()
        self._scan_state = deepcopy(state)
        self._note("scanned", main_boot=self.boot, remote_generation=state["remote_generation"],
                   address=self.identity["address"], address_type=self.identity["type"],
                   matched_devices=len(matches))
        return self

    def pair(self, *, _deadline=None):
        """Pair after a completed scan, without issuing another scan."""
        worker = self._advertised()
        if worker.get("connected") is not False or worker.get("bonded") is not False:
            raise KeyboardConflict("The advertised keyboard already has a Bluetooth peer")
        if self._scan_state is None or self._created or self._pair_pending is not None:
            raise RuntimeError("Complete a fresh keyboard scan before pairing")
        deadline = time.monotonic() + self.timeout if _deadline is None else _deadline
        state = self._status(deadline)
        if (state["remote_generation"] != self._scan_state["remote_generation"]
                or not state["ready"] or any(state[name] for name in
                    ("bonded", "connected", "pairing", "unpairing", "connecting", "scanning"))):
            raise KeyboardConflict("Remote administration changed after keyboard discovery")
        self._post("pair", state, deadline)
        self._finish_pair(deadline)
        self._scan_state = None
        self._disconnected = False
        return self

    def create(self):
        deadline = time.monotonic() + self.timeout
        self.open(_deadline=deadline)
        self.scan(_deadline=deadline)
        self.pair(_deadline=deadline)
        return self

    def assert_paired(self, *, _system=None):
        if not self._created or self._closed or self._disconnected:
            raise RuntimeError("This test has no connected keyboard to verify")
        state = self._status(time.monotonic() + self.timeout, _system=_system)
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

    def checkpoint(self, *, kind="ble_report", _system=None):
        if not self._created or self._closed:
            raise RuntimeError("Pair this keyboard before sending input")
        system = self._system(_system=_system)
        scan = self.dut.last_scan
        if scan is None or scan.dut is not self.dut or scan._session is not self.session:
            raise RuntimeError("BLE keyboard input requires the current TestBed's HDMI scan")
        scan.validate_owners(scan.expected, _system=system)
        state = self.dut.get_state()
        if state.get("expected_boot_id") != self.boot:
            raise KeyboardConflict("The original main restarted before keyboard input")
        return ActivityAction(self.dut, self.session, time.monotonic_ns(), self.boot,
                              deepcopy(scan.initiators), kind, uuid4().hex, None,
                              state=state, scan=scan, testbed=self.bed,
                              owner_boots=dict(scan.owner_boots), owner_duts=dict(scan.owner_duts))

    def report(self, *, keyboard=(), consumer=0):
        keyboard, consumer = _reports(keyboard, consumer, self.keyboard_slots)
        if self._disconnected:
            raise RuntimeError("The disconnected keyboard cannot send input")
        action = self.checkpoint()
        return self._send_report(action, keyboard, consumer)

    def _send_report(self, action, keyboard, consumer):
        action.operation = self.worker.report(keyboard=keyboard, consumer=consumer)
        receipt = action.operation
        if (not isinstance(receipt, dict)
                or type(receipt.get("timestamp_monotonic_ns")) is not int or receipt["timestamp_monotonic_ns"] <= 0
                or not isinstance(receipt.get("keyboard_report"), str)
                or re.fullmatch(r"[0-9a-f]{" + str((2 + self.keyboard_slots) * 2) + "}", receipt["keyboard_report"]) is None
                or not isinstance(receipt.get("consumer_report"), str)
                or re.fullmatch(r"[0-9a-f]{4}", receipt["consumer_report"]) is None):
            raise KeyboardConflict("The keyboard report has no valid physical notification receipt")
        self._note("report", started_monotonic_ns=action.started_ns, keyboard=keyboard,
                   consumer=consumer, **action.operation)
        return action

    def all_up(self):
        return self.report()

    def reports(self):
        """Fence a group of real HID reports with one UI preflight and review."""
        return _KeyboardReports(self)

    def disconnect(self):
        if self._disconnected:
            raise RuntimeError("The keyboard is already disconnected")
        action = self.checkpoint(kind="ble_disconnect")
        action.operation = self.worker.disconnect()
        self._disconnected = True
        self._note("disconnect", started_monotonic_ns=action.started_ns, **action.operation)
        return action

    def _unpair(self, deadline, *, check_health):
        if self._pair_pending is not None:
            # A lost POST reply is never resent. Only a fresh bonded link
            # to this worker can establish ownership for subsequent cleanup.
            self._finish_pair(deadline, check_health=check_health, for_cleanup=True)
        if self._created:
            state = self._status(deadline, check_health=check_health)
            expected = (self._unpair_pending or {}).get("generation", self._pair_generation)
            if state["remote_generation"] != expected:
                raise KeyboardConflict("Remote administration changed before keyboard cleanup")
            if state["bonded"] or state["connected"] or state["pairing"] or state["unpairing"]:
                if self._unpair_pending is None:
                    worker = self.worker.status()
                    if not self._disconnected and worker.get("connected") is True and worker.get("ready") is True:
                        self.worker.report(keyboard=(), consumer=0)
                    self._post("unpair", state, deadline)
                while True:
                    state = self._status(deadline, check_health=check_health)
                    if state["remote_generation"] != self._unpair_pending["generation"]:
                        raise KeyboardConflict("Remote administration changed during keyboard cleanup")
                    if not any(state[name] for name in ("bonded", "connected", "pairing", "unpairing", "connecting")):
                        break
                    self._pause(deadline)
            error = state.get("last_error")
            if type(error) is int and error != 0:
                raise KeyboardConflict("The DUT failed keyboard unpairing (error " + str(error) + ")")
            self._unpair_pending = None
            self._created = False
            self._note("unpaired", main_boot=self.boot, remote_generation=state["remote_generation"],
                       bonded=False, connected=False)
            return state

    def unpair(self):
        """Remove this owned pair while retaining its advertisement for more scans."""
        self._advertised()
        if not self._created or self._disconnected:
            raise RuntimeError("Unpair a connected owned keyboard before reusing its advertisement")
        deadline = time.monotonic() + self.timeout
        completed = self._unpair(deadline, check_health=True)

        def review():
            state = self._status(deadline)
            error = state.get("last_error")
            if (state["remote_generation"] != completed["remote_generation"]
                    or not state["ready"] or any(state[name] for name in
                        ("bonded", "connected", "connecting", "pairing", "unpairing", "scanning"))
                    or type(error) is int and error != 0):
                raise KeyboardConflict("Remote administration changed while the Pi finished disconnecting")

        # DUT completion and BlueZ's property update are independent observers.
        # Wait for the owned link's actual disconnect, without resending Unpair.
        while True:
            review()
            self._remaining(deadline)
            worker = self._advertised()
            self._remaining(deadline)
            if type(worker.get("connected")) is not bool:
                raise KeyboardConflict("The Pi returned invalid keyboard disconnect state")
            if not worker["connected"]:
                review()
                break
            self._pause(deadline)
        self._remaining(deadline)
        result = self.worker.forget_peer()
        if not isinstance(result, dict) or result.get("cleared") is not True:
            raise KeyboardConflict("The Pi did not confirm removal of its owned disconnected peer")
        self._scan_state = None
        self._advertised()
        return self

    def close(self):
        if self._closed:
            return
        deadline = time.monotonic() + self.timeout
        error = None
        try:
            self._unpair(deadline, check_health=False)
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


class _KeyboardReports:
    """Send immediate HID changes while activities may legitimately transition.

    Reports carry fresh host boundaries and the preflight UI snapshot. The
    worker checks its physical bonded connection for every report; existing UI
    reads fence configuration and physical owners before and after the group.
    """

    def __init__(self, keyboard):
        self.keyboard = keyboard
        self._entered = self._closed = False

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("A keyboard report context cannot be reused")
        keyboard = self.keyboard
        system = keyboard._system()
        keyboard.assert_paired(_system=system)
        self.preflight = keyboard.checkpoint(_system=system)
        self.worker, self.identity = keyboard.worker, deepcopy(keyboard.identity)
        self.generation, self.boot, self.group = keyboard._pair_generation, keyboard.boot, keyboard.group
        self.scan, self.bed, self.session = self.preflight.scan, keyboard.bed, keyboard.session
        self._state = deepcopy(self.preflight.state)
        if any(not _generation(self._state.get(field)) for field in
               ("expected_config_revision", "expected_presentation_revision")):
            raise KeyboardConflict("Keyboard reports need current saved configuration revisions")
        self._components = tuple(self.scan.expected)
        self._owner_boots, self._owner_duts = dict(self.preflight.owner_boots), dict(self.preflight.owner_duts)
        self._initiators = dict(self.preflight.initiators)
        self._entered = True
        self._validate()
        return self

    def _owned_worker(self):
        keyboard = self.keyboard
        if (not self._entered or self._closed or not keyboard._created or keyboard._closed
                or keyboard._disconnected or keyboard.worker is not self.worker
                or keyboard.identity != self.identity or keyboard._pair_generation != self.generation
                or keyboard.boot != self.boot or keyboard.group != self.group):
            raise KeyboardConflict("The paired keyboard changed during its report context")

    def _validate(self):
        self._owned_worker()
        keyboard = self.keyboard
        stopped = getattr(self.session, "_stop", None)
        if (keyboard.dut._testbed is not self.bed or keyboard.bed is not self.bed
                or not self.bed._entered or self.bed._closed or self.bed.session is not self.session
                or keyboard.session is not self.session or stopped is not None and stopped.is_set()
                or keyboard.dut.last_scan is not self.scan or self.scan._session is not self.session
                or tuple(self.scan.expected) != self._components
                or self.scan.initiators != self._initiators or self.scan.owner_boots != self._owner_boots
                or self.scan.owner_duts != self._owner_duts
                or any(component._session is not self.session or component.dut._testbed is not self.bed
                       for component in self._components)):
            raise KeyboardConflict("Keyboard reports lost their proven TestBed, scan or owners")
        # CEC workers publish failure before setting _stop. Do not acquire
        # their responder lock between physical HID reports; a CEC response
        # can still be transmitting while independent BLE input is due.

    def _boundary(self):
        keyboard = self.keyboard
        return ActivityAction(keyboard.dut, self.session, time.monotonic_ns(), self.boot,
                              dict(self._initiators), "ble_report", uuid4().hex, None,
                              state=deepcopy(self._state), scan=self.scan, testbed=self.bed,
                              owner_boots=dict(self._owner_boots), owner_duts=dict(self._owner_duts))

    def report(self, *, keyboard=(), consumer=0):
        keyboard, consumer = _reports(keyboard, consumer, self.keyboard.keyboard_slots)
        self._validate()
        return self.keyboard._send_report(self._boundary(), keyboard, consumer)

    def all_up(self):
        return self.report()

    def _confirm(self):
        self.session.assert_healthy()
        self._validate()
        system = self.keyboard._system()
        self.keyboard.assert_paired(_system=system)
        after = self.keyboard.checkpoint(_system=system)
        if (after.scan is not self.scan or after.boot != self.boot
                or after.owner_boots != self._owner_boots or after.owner_duts != self._owner_duts
                or after.initiators != self._initiators
                or any(after.state.get(field) != self._state.get(field) for field in
                       ("expected_boot_id", "expected_config_revision", "expected_presentation_revision"))):
            raise KeyboardConflict("Keyboard reports changed configuration, boot or physical owners")
        self._validate()

    def __exit__(self, exc_type, exc, traceback):
        failures = []
        try:
            # Release our actual HID state before slower HTTP confirmation,
            # including when the test body or its scan proof failed.
            self._owned_worker()
            self.keyboard._send_report(self._boundary(), [], 0)
        except BaseException as error:
            failures.append(error)
        try:
            self._confirm()
        except BaseException as error:
            failures.append(error)
        self._closed = True
        if failures:
            if exc is None:
                raise failures[0]
            if hasattr(exc, "add_note"):
                exc.add_note("Keyboard report cleanup or identity review failed: "
                             + ", ".join(type(error).__name__ for error in failures))
        return False
