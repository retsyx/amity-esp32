"""Remote button state through the shipping authenticated key API."""

from copy import deepcopy
from dataclasses import dataclass, field
from enum import IntEnum
import time

from .activity import ActivityAction
from .dut import _positive_seconds
from .trace import event


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
    """One bounded button press and its original physical wire evidence."""

    key: int = 0
    duration_ms: int = 0
    trace: dict = field(default_factory=dict, repr=False)

    def _validate(self, *, standby=False):
        if (self.dut._testbed is not self.testbed or self.testbed.session is not self.session
                or self.testbed._closed or not self.testbed._entered or self.dut.last_scan is not self.scan):
            raise AssertionError("The key action belongs to a different active TestBed or scan")
        self.session.assert_healthy()
        self.scan.validate_owners(self.scan.expected)
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

    def assert_received_by(self, component, *, emitted_key=None, timeout=None):
        """Require a fresh directed press and final release on the correct wire.

        Held-key repetition is legitimate. Original kernel frames must contain
        this key and an operand-free release after its final press; virtual,
        stale, rewritten or dropped RX cannot qualify. Any matching press to
        another fixture recipient fails, including on another Amity's wire.
        """
        timeout = self.dut._runtime.cec_timeout if timeout is None else timeout
        _positive_seconds(timeout, "Key assertion timeout")
        emitted_key = self.key if emitted_key is None else _key(emitted_key)
        self._validate()
        session, started, source, _ = component._wire_context(self)
        if component not in self.scan.expected:
            raise AssertionError("The key recipient is outside the scanned fixture")
        deadline_ns = self.started_ns + int((self.duration_ms / 1000 + timeout) * 1_000_000_000)

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
                if record.get("opcode") == 0x44 and operands == [f"{emitted_key:02x}"]:
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
                self._validate()
                component._wire_context(self)
                proof = inspect()
                if proof is not None:
                    press, release, count = proof
                    self.trace.update(component=component.name, owner_id=component.dut.identity.device_id,
                                      emitted_key=emitted_key,
                                      gpio=component.gpio, press_frame=press["frame"], release_frame=release["frame"],
                                      deadline_monotonic_ns=deadline_ns,
                                      press_wire_timestamp_ns=press["wire_timestamp_ns"],
                                      release_wire_timestamp_ns=release["wire_timestamp_ns"], presses=count, released=True)
                    event("key.assert_passed", dut=self.dut.name, **self.trace)
                    return self
            remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
            if remaining <= 0:
                raise TimeoutError("The recipient did not receive a fresh physical key press and release")
            with component._power_condition:
                component._power_condition.wait(min(remaining, 0.02))

    def wait_for_standby(self):
        """Require the Power button to select and settle system Standby."""
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


def press_key(dut, key, *, duration_ms=0):
    action = _prepare_key(dut, key, duration_ms)
    result = dut.request("/api/key", {"key": key, "duration_ms": duration_ms})
    if result.get("ok") is not True:
        raise AssertionError("Amity did not accept the key press")
    event("key.pressed", dut=dut.name, key=key, duration_ms=duration_ms)
    return action


def _physical_key_frames(action, component):
    """Inspect original wire evidence without HTTP while the key handler waits."""
    presses, releases = [], []
    for record in action.session.records:
        stamp, gpio = record.get("wire_timestamp_ns", 0), record.get("gpio")
        if (record.get("direction") != "rx" or record.get("transport") != "wire"
                or stamp < action.started_ns or record.get("source") != action.initiators.get(gpio)):
            continue
        raw = record.get("frame", "")
        operands = raw.split(":")[2:]
        if record.get("opcode") == 0x44 and operands == [f"{action.key:02x}"]:
            if gpio != component.gpio or record.get("destination") != component.address.logical:
                raise AssertionError("The key was sent to the wrong physical recipient")
            if (record.get("component") == component.name and not record.get("dropped")
                    and record.get("handled_frame", raw) == raw):
                presses.append(record)
        elif (record.get("opcode") == 0x45 and not operands and gpio == component.gpio
                and record.get("destination") == component.address.logical
                and record.get("component") == component.name and not record.get("dropped")
                and record.get("handled_frame", raw) == raw):
            releases.append(record)
    return sorted(presses, key=lambda row: row["wire_timestamp_ns"]), sorted(
        releases, key=lambda row: row["wire_timestamp_ns"])


def press_key_disconnected(dut, key, component, *, duration_ms=3000):
    """Lose the HTTP caller during a physically admitted, bounded key hold.

    Admission is proven by the first original directed press, never by an HTTP
    acknowledgment. After disconnecting, require the final release by the
    original hold/CEC deadline and observe an additional CEC window for stray
    presses. Every owner/boot fence remains in force.
    """
    if type(duration_ms) is not int or not 1000 <= duration_ms <= 5000:
        raise ValueError("A disconnected hold needs a duration from 1000 to 5000 milliseconds")
    if _key(key) in (Key.POWER, Key.PLAY_PAUSE):
        raise ValueError("A disconnected hold needs an untranslated source or audio key")
    action = _prepare_key(dut, key, duration_ms)
    action._validate()
    if component not in action.scan.expected:
        raise AssertionError("The key recipient is outside the scanned fixture")
    component._wire_context(action)  # Resolve owner proof before occupying HTTP.
    backend = getattr(action.session, "_backends", {}).get(component.gpio)
    if backend is None:
        raise AssertionError("The disconnected hold has no physical wire clock")
    # Identity reads above can be slow. The physical interval begins immediately
    # before sending the key, after all of those reads have finished.
    action.started_ns = time.monotonic_ns()
    action.trace.update(started_monotonic_ns=action.started_ns, disconnected=True)
    hold_deadline = action.started_ns + duration_ms * 1_000_000
    action.trace.update(hold_deadline_monotonic_ns=hold_deadline)

    def first_press():
        while True:
            action.session.assert_healthy()
            if component._session is not action.session or action.session._stop.is_set():
                raise AssertionError("The CEC session ended before client disconnection")
            presses, releases = _physical_key_frames(action, component)
            if releases:
                raise AssertionError("The key stream ended before the client disconnected")
            if time.monotonic_ns() >= hold_deadline:
                raise TimeoutError("No physical press arrived while the HTTP hold was still pending")
            if presses and component.pressed_key == action.key:
                action.trace["first_press_wire_timestamp_ns"] = presses[0]["wire_timestamp_ns"]
                return
            with component._power_condition:
                component._power_condition.wait(min((hold_deadline - time.monotonic_ns()) / 1e9, .02))

    dut._disconnect_key_request({"key": action.key, "duration_ms": duration_ms}, first_press,
                                timeout=duration_ms / 1000)
    disconnected_ns = time.monotonic_ns()
    if disconnected_ns >= hold_deadline:
        raise AssertionError("The HTTP connection did not close before the bounded hold ended")
    action.trace["disconnected_monotonic_ns"] = disconnected_ns
    action.assert_received_by(component)
    presses, releases = _physical_key_frames(action, component)
    repeats = [row for row in presses if row["wire_timestamp_ns"] > disconnected_ns]
    if not repeats:
        raise AssertionError("The bounded hold did not repeat after the client disconnected")
    action.trace["repeat_after_disconnect_wire_timestamp_ns"] = repeats[0]["wire_timestamp_ns"]
    # The first release terminates this stream. A later press/release pair
    # cannot hide a restart by becoming assert_received_by's final pair.
    release_ns = releases[0]["wire_timestamp_ns"]
    # The API starts its bounded wait after admitting the request, not after
    # the first press completes. Request start is therefore the conservative
    # duration boundary. Check release completion only: Linux RX timestamps
    # cannot establish when that frame's first edge began.
    if release_ns < hold_deadline:
        raise AssertionError("The disconnected hold released before its requested duration")
    if any(row["wire_timestamp_ns"] <= disconnected_ns for row in releases):
        raise AssertionError("The key stream ended before the client disconnected")
    if any(row["wire_timestamp_ns"] > release_ns for row in presses):
        raise AssertionError("A key press followed the disconnected hold's final release")
    observed_until = release_ns + int(dut._runtime.cec_timeout * 1e9)
    drain_deadline = observed_until + int(dut._runtime.cec_timeout * 1e9)
    while True:
        action.session.assert_healthy()
        if component._session is not action.session or action.session._stop.is_set():
            raise AssertionError("The CEC session ended during disconnected-key observation")
        presses, _ = _physical_key_frames(action, component)
        if any(row["wire_timestamp_ns"] > release_ns for row in presses):
            raise AssertionError("A key press followed the disconnected hold's final release")
        now = time.monotonic_ns()
        if now >= observed_until:
            idle = getattr(action.session, "_wire_idle_since_ns", {})
            wires = {item.gpio for item in action.scan.expected}
            if all(idle.get(gpio, 0) >= observed_until for gpio in wires):
                action.trace.update(observed_wires=sorted(wires),
                                    idle_receive_since_ns={str(gpio): idle[gpio] for gpio in sorted(wires)})
                break
            if now >= drain_deadline:
                raise TimeoutError("CEC receive coverage did not drain after the disconnected hold")
        with component._power_condition:
            component._power_condition.wait(.02)
    action._validate()
    # HTTP owner proof may take time; inspect all captured traffic once more.
    presses, _ = _physical_key_frames(action, component)
    if any(row["wire_timestamp_ns"] > release_ns for row in presses):
        raise AssertionError("A key press followed the disconnected hold's final release")
    action.trace.update(observation_end_monotonic_ns=observed_until, cleanup_verified=True)
    event("key.disconnected_cleanup.assert_passed", dut=dut.name, **action.trace)
    return action
