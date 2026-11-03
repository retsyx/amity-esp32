"""Original physical CEC evidence for reports from a paired BLE keyboard."""

import threading
import time

from .keys import Key
from .trace import event


class BLEInputEvidence:
    """Observe keyboard input at its actual CEC recipients, before fault hooks.

    The keyboard sends complete HID states over BLE. This class never submits
    an HTTP key command. Existing UI reads fence the activity, configuration,
    main boot and proven owners; original wire frames establish key behavior.
    """

    def __init__(self, dut, scan, *components):
        self.dut, self.scan = dut, scan
        self.bed, self.session = getattr(dut, "_testbed", None), scan._session
        self.components = tuple(dict.fromkeys(scan.expected))
        if (scan.dut is not dut or not self.components
                or any(component not in self.components for component in components)):
            raise ValueError("BLE input evidence needs this main's scanned equipment")
        self._condition = threading.Condition()
        self._frames, self._seen = [], set()
        self._invalid_clock = self._entered = self._closed = False
        self._checks = []

    def _validate(self):
        if (self.bed is None or not self.bed._entered or self.bed._closed
                or self.dut._testbed is not self.bed or self.bed.session is not self.session
                or self.dut.last_scan is not self.scan or self.scan._session is not self.session
                or self.session._stop.is_set()
                or any(component._session is not self.session for component in self.components)):
            raise AssertionError("BLE input evidence lost its active TestBed or scan")
        self.session.assert_healthy()
        self.scan.validate_owners(self.components)
        state = self.dut.get_state()
        if (state.get("expected_boot_id") != self.scan._boot
                or type(state.get("current_activity")) is not int or state["current_activity"] < 0
                or state.get("activity_settling") is not False):
            raise AssertionError("BLE input needs the same ready, active owner boot")
        if self._entered and (self.scan.owner_boots != self._owner_boots
                or self.scan.initiators != self._initiators
                or any(state.get(name) != self._state.get(name) for name in
                       ("current_activity", "expected_config_revision", "expected_presentation_revision"))):
            raise AssertionError("The activity, configuration or input owner changed")
        for component in self.components:
            context = component._wire_context(time.monotonic_ns())
            if context[0] is not self.session or context[2] != self.scan.initiators.get(component.gpio):
                raise AssertionError("BLE input has no matching physical CEC initiator")
        return state

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("BLE input evidence cannot be reused")
        self._state = self._validate()
        self._owner_boots, self._initiators = dict(self.scan.owner_boots), dict(self.scan.initiators)
        with self.session._lock:
            self._started = time.monotonic_ns()
            for component in self.components:
                component._wire_observers.add(self._observe)
        self._entered = True
        self._report = {"kind": "ble_keyboard_input", "started_monotonic_ns": self._started,
                        "owner_boots": dict(self._owner_boots), "checks": self._checks}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self._report)
        return self

    def _observe(self, component, frame, transport):
        if (transport != "wire" or frame.opcode not in (0x44, 0x45)
                or frame.source != self._initiators.get(component.gpio)):
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                key = component.gpio, frame.timestamp_ns, frame.raw
                if key not in self._seen:
                    self._seen.add(key)
                    self._frames.append((component.gpio, frame))
            self._condition.notify_all()

    def _live(self):
        self.session.assert_healthy()
        if (not self._entered or self._closed or self.session._stop.is_set()
                or any(component._session is not self.session for component in self.components)):
            raise AssertionError("The CEC session ended during BLE input evidence")

    def _snapshot(self):
        self._live()
        with self._condition:
            if self._invalid_clock:
                raise AssertionError("BLE key evidence has no physical wire timestamp")
            return tuple(self._frames)

    def _wait(self, predicate, *, deadline_ns, message):
        while True:
            result = predicate(self._snapshot())
            if result is not None:
                return result
            remaining = (deadline_ns - time.monotonic_ns()) / 1e9
            if remaining <= 0:
                raise TimeoutError(message)
            with self._condition:
                self._condition.wait(min(remaining, .02))

    def _handled(self, component, frame):
        return any(record.get("component") == component.name and record.get("gpio") == component.gpio
                   and record.get("direction") == "rx" and record.get("transport") == "wire"
                   and record.get("wire_timestamp_ns") == frame.timestamp_ns
                   and record.get("frame") == frame.raw.hex(":") and not record.get("dropped")
                   and record.get("handled_frame", record.get("frame")) == frame.raw.hex(":")
                   for record in self.session.records)

    def _press(self, component, key, *, after, deadline_ns):
        def find(frames):
            matches = []
            for gpio, frame in frames:
                if not after < frame.timestamp_ns <= deadline_ns or frame.opcode != 0x44:
                    continue
                if frame.operands != bytes([key]):
                    continue
                if gpio != component.gpio or frame.destination != component.address.logical:
                    raise AssertionError("BLE key reached the wrong physical recipient")
                if self._handled(component, frame) and component.pressed_key == key:
                    matches.append(frame)
            return min(matches, key=lambda frame: frame.timestamp_ns) if matches else None
        return self._wait(find, deadline_ns=deadline_ns, message="No fresh physical BLE key press")

    def _release(self, component, *, after, deadline_ns):
        def find(frames):
            matches = [frame for gpio, frame in frames if gpio == component.gpio
                       and frame.destination == component.address.logical and frame.opcode == 0x45
                       and after < frame.timestamp_ns <= deadline_ns]
            if any(frame.operands for frame in matches):
                raise AssertionError("A CEC release must have no key operand")
            matches = [frame for frame in matches if self._handled(component, frame)]
            return min(matches, key=lambda frame: frame.timestamp_ns) if matches and component.pressed_key is None else None
        return self._wait(find, deadline_ns=deadline_ns, message="No fresh physical BLE key release")

    def _drain(self, through):
        deadline_ns = max(time.monotonic_ns(), through) + self._budget()
        wires = {component.gpio for component in self.components}
        def drained(_):
            with self.session._lock:
                covered = all(self.session._wire_idle_since_ns.get(gpio, 0) >= through for gpio in wires)
            return True if covered and time.monotonic_ns() >= through else None
        self._wait(drained, deadline_ns=deadline_ns, message="BLE key receive coverage did not drain")
        return {str(gpio): self.session._wire_idle_since_ns[gpio] for gpio in sorted(wires)}

    def _budget(self):
        return int(self.dut._runtime.cec_timeout * 1e9)

    def _begin(self, keyboard, kind):
        self._validate()
        keyboard.assert_paired()
        record = {"kind": kind, "started_monotonic_ns": time.monotonic_ns(), "passed": False}
        self._checks.append(record)
        return record

    def _finish(self, record, *, routes, released):
        through = time.monotonic_ns()
        record["idle_receive_since_ns"] = self._drain(through)
        self._validate()
        # Owner proof can take time. Drain and inspect that interval too, so a
        # delayed press cannot hide behind an earlier successful release.
        through = time.monotonic_ns()
        record["idle_receive_since_ns"] = self._drain(through)
        frames = [(gpio, frame) for gpio, frame in self._snapshot()
                  if record["started_monotonic_ns"] < frame.timestamp_ns <= through]
        for gpio, frame in frames:
            if frame.opcode != 0x44:
                continue
            if len(frame.operands) != 1 or frame.operands[0] not in routes:
                raise AssertionError("BLE input produced an unexpected key command")
            recipient = routes[frame.operands[0]]
            if (gpio, frame.destination) != (recipient.gpio, recipient.address.logical):
                raise AssertionError("BLE key reached the wrong physical recipient")
        for component, release in released:
            if any(gpio == component.gpio and frame.destination == component.address.logical
                   and frame.opcode == 0x44 and frame.timestamp_ns > release.timestamp_ns
                   for gpio, frame in frames):
                raise AssertionError("A BLE key press followed its final release")
            if component.pressed_key is not None:
                raise AssertionError("The BLE key recipient did not finish released")
        record["frames"] = [{"gpio": gpio, "frame": frame.raw.hex(":"),
                             "wire_timestamp_ns": frame.timestamp_ns} for gpio, frame in frames]
        record.update(observation_end_monotonic_ns=through, passed=True)
        event("ble_input.assert_passed", dut=self.dut.name, **record)
        return self

    @staticmethod
    def _all_up(keyboard, primary):
        try:
            keyboard.all_up()
        except Exception as cleanup_error:
            if primary is None:
                raise
            if hasattr(primary, "add_note"):
                primary.add_note("BLE all-buttons-up cleanup failed: " + type(cleanup_error).__name__)

    def assert_tap(self, keyboard, component, *, usage=0x28, key=Key.SELECT):
        """Require a keyboard usage's fresh physical press and final release."""
        if component not in self.components:
            raise ValueError("The BLE key recipient is outside this scan")
        record = self._begin(keyboard, "tap")
        primary = None
        try:
            started = time.monotonic_ns()
            keyboard.report(keyboard=(usage,), consumer=0)
            press = self._press(component, int(key), after=started, deadline_ns=started + self._budget())
            up = time.monotonic_ns()
            keyboard.all_up()
            release = self._release(component, after=up, deadline_ns=up + self._budget())
            if release.timestamp_ns <= press.timestamp_ns:
                raise AssertionError("BLE key release preceded its press")
            record.update(component=component.name, key=int(key), usage=usage,
                          press_frame=press.raw.hex(":"), press_wire_timestamp_ns=press.timestamp_ns,
                          release_frame=release.raw.hex(":"), release_wire_timestamp_ns=release.timestamp_ns)
            return self._finish(record, routes={int(key): component}, released=((component, release),))
        except BaseException as error:
            primary = error
            raise
        finally:
            self._all_up(keyboard, primary)

    def assert_overlapping_buttons(self, keyboard, *, volume, navigation):
        """Right's release must leave Volume Up's independent stream running."""
        if (volume not in self.components or navigation not in self.components or volume is navigation
                or (volume.gpio, volume.address.logical) == (navigation.gpio, navigation.address.logical)):
            raise ValueError("Overlapping buttons need distinct physical recipients")
        record = self._begin(keyboard, "overlapping_buttons")
        primary = None
        try:
            started = time.monotonic_ns()
            deadline = started + self._budget()
            keyboard.report(keyboard=(), consumer=0xE9)
            first = self._press(volume, Key.VOLUME_UP, after=started, deadline_ns=deadline)
            both = time.monotonic_ns()
            keyboard.report(keyboard=(0x4F,), consumer=0xE9)
            right = self._press(navigation, Key.RIGHT, after=both, deadline_ns=deadline)
            right_up = time.monotonic_ns()
            keyboard.report(keyboard=(), consumer=0xE9)
            released_right = self._release(navigation, after=right_up, deadline_ns=deadline)
            uncertainty = getattr(self.session._backends[navigation.gpio], "clock_uncertainty_ns", 0)
            if type(uncertainty) is not int or uncertainty < 0:
                raise AssertionError("BLE key wire clock uncertainty is unavailable")
            continued = self._press(volume, Key.VOLUME_UP,
                                    after=released_right.timestamp_ns + 2 * uncertainty, deadline_ns=deadline)
            final_up = time.monotonic_ns()
            keyboard.all_up()
            final_release = self._release(volume, after=final_up, deadline_ns=final_up + self._budget())
            coverage = time.monotonic_ns()
            self._drain(coverage)
            frames = self._snapshot()
            if any(gpio == volume.gpio and frame.destination == volume.address.logical and frame.opcode == 0x45
                   and started < frame.timestamp_ns < final_up for gpio, frame in frames):
                raise AssertionError("Releasing Right also ended the held Volume Up stream")
            if any(gpio == navigation.gpio and frame.destination == navigation.address.logical
                   and frame.opcode == 0x44 and frame.operands == bytes([Key.RIGHT])
                   and frame.timestamp_ns > released_right.timestamp_ns for gpio, frame in frames):
                raise AssertionError("Right restarted after its final release")
            record.update(volume_component=volume.name, navigation_component=navigation.name,
                          first_volume_press_wire_timestamp_ns=first.timestamp_ns,
                          right_press_wire_timestamp_ns=right.timestamp_ns,
                          right_release_wire_timestamp_ns=released_right.timestamp_ns,
                          continuing_volume_press_wire_timestamp_ns=continued.timestamp_ns,
                          final_all_up_monotonic_ns=final_up,
                          volume_release_wire_timestamp_ns=final_release.timestamp_ns)
            return self._finish(record, routes={Key.VOLUME_UP: volume, Key.RIGHT: navigation},
                                released=((volume, final_release), (navigation, released_right)))
        except BaseException as error:
            primary = error
            raise
        finally:
            self._all_up(keyboard, primary)

    def assert_disconnect_cleanup(self, keyboard, component):
        """Dropping the BLE link must release its live stream and not restart it."""
        if component not in self.components:
            raise ValueError("The BLE key recipient is outside this scan")
        record = self._begin(keyboard, "disconnect_cleanup")
        primary, disconnected = None, False
        try:
            started = time.monotonic_ns()
            keyboard.report(keyboard=(), consumer=0xE9)
            first = self._press(component, Key.VOLUME_UP, after=started, deadline_ns=started + self._budget())
            repeated = self._press(component, Key.VOLUME_UP, after=first.timestamp_ns,
                                   deadline_ns=started + self._budget())
            lost = time.monotonic_ns()
            keyboard.disconnect()
            disconnected = True
            release = self._release(component, after=lost, deadline_ns=lost + self._budget())
            observed_until = release.timestamp_ns + self._budget()
            self._drain(observed_until)
            frames = self._snapshot()
            releases = [frame for gpio, frame in frames if gpio == component.gpio
                        and frame.destination == component.address.logical and frame.opcode == 0x45
                        and frame.timestamp_ns > started]
            if any(frame.timestamp_ns <= lost for frame in releases):
                raise AssertionError("The held BLE key ended before disconnection")
            if any(gpio == component.gpio and frame.destination == component.address.logical
                   and frame.opcode == 0x44 and frame.operands == bytes([Key.VOLUME_UP])
                   and frame.timestamp_ns > release.timestamp_ns for gpio, frame in frames):
                raise AssertionError("The BLE key restarted after disconnect cleanup")
            keyboard.assert_disconnected()
            record.update(component=component.name, first_press_wire_timestamp_ns=first.timestamp_ns,
                          repeat_wire_timestamp_ns=repeated.timestamp_ns,
                          disconnected_monotonic_ns=lost, release_wire_timestamp_ns=release.timestamp_ns,
                          no_press_through_monotonic_ns=observed_until)
            return self._finish(record, routes={Key.VOLUME_UP: component}, released=((component, release),))
        except BaseException as error:
            primary = error
            raise
        finally:
            if not disconnected:
                self._all_up(keyboard, primary)

    def __exit__(self, exc_type, exc, traceback):
        if self._entered:
            with self.session._lock:
                for component in self.components:
                    component._wire_observers.discard(self._observe)
        self._closed = True
        if exc_type is None and (not self._checks or not all(check["passed"] for check in self._checks)):
            raise RuntimeError("BLE input evidence was not asserted")
