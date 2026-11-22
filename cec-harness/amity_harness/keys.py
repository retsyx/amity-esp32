"""Physical CEC evidence for remote keys from shipping input surfaces.

Keys come from the paired BLE remote, Matter or HomeKit. The web UI has no
key control, so no HTTP request supplies a key.
"""

from copy import deepcopy
from dataclasses import dataclass, field
from enum import IntEnum
import time

from .activity import ActivityAction
from .dut import _positive_seconds
from .trace import event


# A press frame with one operand occupies about 93 ms of bus time including
# the initiator's signal-free time. A hold at least this long therefore spans
# several press frames, and its owner must have emitted held repetitions.
HOLD_REPEAT_MIN_MS = 400


class Key(IntEnum):
    SELECT = 0x00
    UP = 0x01
    DOWN = 0x02
    LEFT = 0x03
    RIGHT = 0x04
    POWER = 0x40
    VOLUME_UP = 0x41
    VOLUME_DOWN = 0x42
    MUTE = 0x43
    PLAY = 0x44
    STOP = 0x45
    PAUSE = 0x46
    PLAY_PAUSE = 0x61


def _key(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 255:
        raise ValueError("A key must be an integer from 0 to 255")
    return int(value)


@dataclass
class KeyAction(ActivityAction):
    """One bounded button press and its original physical wire evidence.

    ``complete_tap`` marks an input surface that delivers a tap as one
    command, so a tap must emit exactly one press. A remote's HID down and
    up are separate reports; held repetition between them depends on their
    delivery timing.
    """

    key: int = 0
    duration_ms: int = 0
    input_completed_ns: int | None = None
    complete_tap: bool = True
    trace: dict = field(default_factory=dict, repr=False)

    def _validate(self, *, standby=False, _system=None):
        if (self.dut._testbed is not self.testbed or self.testbed.session is not self.session
                or self.testbed._closed or not self.testbed._entered or self.dut.last_scan is not self.scan):
            raise AssertionError("The key action belongs to a different active TestBed or scan")
        self.session.assert_healthy()
        self.scan.validate_owners(self.scan.expected, _system=_system)
        current = self.dut.get_state()
        if current.get("expected_boot_id") != self.boot:
            raise AssertionError("DUT restarted during the key action")
        for name in ("expected_config_revision", "expected_presentation_revision"):
            if current.get(name) != self.state.get(name):
                raise AssertionError("Activity configuration changed during the key action")
        target = -1 if standby else self.state.get("current_activity")
        if current.get("current_activity") != target or current.get("activity_settling", False):
            raise AssertionError("The selected activity changed during the key action")
        return current

    def assert_received_by(self, component, *, timeout=None):
        """Require a fresh directed press and final release on the correct wire.

        A complete tap command emits exactly one press: every key press uses
        one wire attempt. A hold of at least ``HOLD_REPEAT_MIN_MS`` must show
        held repetition. Original kernel frames must contain this key and an
        operand-free release after its final press; virtual, stale, rewritten
        or dropped RX cannot qualify. Any matching press to another fixture
        recipient fails, including on another Amity's wire.
        """
        timeout = self.dut._runtime.cec_timeout if timeout is None else timeout
        _positive_seconds(timeout, "Key assertion timeout")
        system = self.dut.get_system()
        self._validate(_system=system)
        session, started, source, _ = component._wire_context(self, _system=system)
        if component not in self.scan.expected:
            raise AssertionError("The key recipient is outside the scanned fixture")
        # Freshness starts before the first input change. The CEC budget
        # starts when the final input change (a remote's key up) is sent,
        # after any requested hold; adding the hold again would double-count it.
        deadline_ns = (self.input_completed_ns + int(timeout * 1_000_000_000)
                       if self.input_completed_ns is not None else
                       self.started_ns + int((self.duration_ms / 1000 + timeout) * 1_000_000_000))
        self.trace["deadline_monotonic_ns"] = deadline_ns
        if self.input_completed_ns is not None:
            self.trace["cec_budget_ms"] = timeout * 1000

        def inspect():
            presses, releases = [], []
            for record in session.records:
                gpio = record.get("gpio")
                if (record.get("direction") != "rx" or record.get("transport") != "wire"
                        or not started <= record.get("wire_timestamp_ns", 0) <= deadline_ns
                        or record.get("source") != self.initiators.get(gpio)):
                    continue
                raw = record.get("frame", "")
                operands = raw.split(":")[2:]
                if record.get("opcode") == 0x44 and operands == [f"{self.key:02x}"]:
                    if gpio != component.gpio or record.get("destination") != component.address.logical:
                        raise AssertionError("The key was sent to the wrong physical recipient")
                    if (record.get("component") == component.name and not record.get("dropped")
                            and record.get("handled_frame", raw) == raw):
                        presses.append(record)
                elif (record.get("opcode") == 0x45 and not operands and gpio == component.gpio
                      and record.get("source") == source and record.get("destination") == component.address.logical
                      and record.get("component") == component.name and not record.get("dropped")
                      and record.get("handled_frame", raw) == raw):
                    releases.append(record)
            if not presses:
                return None
            final_press = max(presses, key=lambda row: row["wire_timestamp_ns"])
            release = next((row for row in releases if row["wire_timestamp_ns"] > final_press["wire_timestamp_ns"]), None)
            return (final_press, release, len(presses)) if release is not None and component.pressed_key is None else None

        while True:
            session.assert_healthy()
            if component._session is not session or session._stop.is_set():
                raise AssertionError("The CEC session ended during the key assertion")
            proof = inspect()
            if proof is not None:
                system = self.dut.get_system()
                self._validate(_system=system)
                component._wire_context(self, _system=system)
                proof = inspect()
                if proof is not None:
                    press, release, count = proof
                    self.trace["presses"] = count
                    if self.complete_tap and not self.duration_ms and count != 1:
                        raise AssertionError("A key tap emitted more than one physical press")
                    if self.duration_ms >= HOLD_REPEAT_MIN_MS and count < 2:
                        raise AssertionError("A held key produced no repeated physical presses")
                    self.trace.update(component=component.name, owner_id=component.dut.identity.device_id,
                                      gpio=component.gpio, press_frame=press["frame"], release_frame=release["frame"],
                                      deadline_monotonic_ns=deadline_ns,
                                      press_wire_timestamp_ns=press["wire_timestamp_ns"],
                                      release_wire_timestamp_ns=release["wire_timestamp_ns"], presses=count, released=True)
                    if self.input_completed_ns is not None:
                        self.trace.update(press_after_input_ms=round((press["wire_timestamp_ns"] - self.input_completed_ns) / 1e6, 3),
                                          release_after_input_ms=round((release["wire_timestamp_ns"] - self.input_completed_ns) / 1e6, 3))
                    event("key.assert_passed", dut=self.dut.name, **self.trace)
                    return self
            remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
            if remaining <= 0:
                raise TimeoutError("The recipient did not receive a fresh physical key press and release")
            with component._power_condition:
                component._power_condition.wait(min(remaining, 0.02))

    def wait_for_standby(self):
        """Require the remote's Power button to select and settle system Standby."""
        if self.key != Key.POWER:
            raise ValueError("Standby evidence requires a Power button")
        self.dut.wait_for(lambda state: state.get("current_activity") == -1
                          and not state.get("activity_settling", False), timeout=self.dut._runtime.timeout)
        self._validate(standby=True)
        self.trace["standby"] = True
        return self


def _prepare_key(dut, key, duration_ms):
    key = _key(key)
    if type(duration_ms) is not int or not 0 <= duration_ms <= 5000:
        raise ValueError("Key duration must be an integer from 0 to 5000 milliseconds")
    bed, scan = dut._testbed, dut.last_scan
    if (bed is None or bed.session is None or bed._closed or not bed._entered
            or scan is None or scan.dut is not dut or scan._session is not bed.session):
        raise RuntimeError("Press keys inside the main's active, scanned TestBed")
    scan.validate_owners(scan.expected)
    state = dut.get_state()
    if state.get("expected_boot_id") != scan._boot:
        raise AssertionError("DUT restarted after its HDMI scan")
    action = KeyAction(dut, bed.session, time.monotonic_ns(), scan._boot, deepcopy(scan.initiators),
                       "key", "", None, state=state, scan=scan, testbed=bed,
                       owner_boots=dict(scan.owner_boots), owner_duts=dict(scan.owner_duts),
                       key=key, duration_ms=duration_ms)
    action.trace = {"key": key, "started_monotonic_ns": action.started_ns, "boot": action.boot,
                    "owner_boots": dict(action.owner_boots), "duration_ms": duration_ms}
    dut.key_operations.append(action.trace)
    return action
