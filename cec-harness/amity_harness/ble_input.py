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

    def _validate(self, *, allow_activity_change=False):
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
        stable = ("expected_config_revision", "expected_presentation_revision")
        if not allow_activity_change:
            stable += ("current_activity",)
        if self._entered and (self.scan.owner_boots != self._owner_boots
                or self.scan.initiators != self._initiators
                or any(state.get(name) != self._state.get(name) for name in stable)):
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

    def _change(self, callback, record, phase, *, kind="ble_report"):
        # The keyboard performs authenticated UI preflight before capturing its
        # action boundary. Only the subsequent BLE submission and CEC response
        # consume the ordinary CEC budget, independently for each input change.
        before = time.monotonic_ns()
        action = callback()
        marker = getattr(action, "started_ns", None)
        if (type(marker) is not int or not before <= marker <= time.monotonic_ns()
                or getattr(action, "dut", None) is not self.dut
                or getattr(action, "session", None) is not self.session
                or getattr(action, "scan", None) is not self.scan
                or getattr(action, "testbed", None) is not self.bed
                or getattr(action, "kind", None) != kind
                or getattr(action, "boot", None) != self.scan._boot
                or getattr(action, "owner_boots", None) != self._owner_boots
                or getattr(action, "initiators", None) != self._initiators):
            raise AssertionError("BLE input has no current post-preflight action boundary")
        if any(action.state.get(name) != self._state.get(name) for name in
               ("expected_boot_id", "current_activity", "expected_config_revision", "expected_presentation_revision")):
            raise AssertionError("The activity changed during BLE report preflight")
        deadline = marker + self._budget()
        record.setdefault("input_changes", []).append({"phase": phase,
            "started_monotonic_ns": marker, "deadline_monotonic_ns": deadline})
        return marker, deadline

    def _finish(self, record, *, routes, released, release_once=False, validate_frames=None):
        through = time.monotonic_ns()
        record["idle_receive_since_ns"] = self._drain(through)
        self._validate()
        # Owner proof can take time. Drain and inspect that interval too, so a
        # delayed press cannot hide behind an earlier successful release.
        through = time.monotonic_ns()
        record["idle_receive_since_ns"] = self._drain(through)
        frames = [(gpio, frame) for gpio, frame in self._snapshot()
                  if record["started_monotonic_ns"] < frame.timestamp_ns <= through]
        if validate_frames is not None:
            validate_frames(frames)
        for gpio, frame in frames:
            if frame.opcode != 0x44:
                continue
            if len(frame.operands) != 1 or frame.operands[0] not in routes:
                raise AssertionError("BLE input produced an unexpected key command")
            recipient = routes[frame.operands[0]]
            if (gpio, frame.destination) != (recipient.gpio, recipient.address.logical):
                raise AssertionError("BLE key reached the wrong physical recipient")
        for component, release in released:
            if release_once and sum(gpio == component.gpio and frame.destination == component.address.logical
                                    and frame.opcode == 0x45 for gpio, frame in frames) != 1:
                raise AssertionError("Shared keys must produce one final CEC release")
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
            started, deadline = self._change(lambda: keyboard.report(keyboard=(usage,), consumer=0), record, "tap_down")
            press = self._press(component, int(key), after=started, deadline_ns=deadline)
            up, deadline = self._change(keyboard.all_up, record, "tap_up")
            release = self._release(component, after=up, deadline_ns=deadline)
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
            started, deadline = self._change(lambda: keyboard.report(keyboard=(), consumer=0xE9), record, "volume_down")
            first = self._press(volume, Key.VOLUME_UP, after=started, deadline_ns=deadline)
            both, deadline = self._change(lambda: keyboard.report(keyboard=(0x4F,), consumer=0xE9), record, "right_down")
            right = self._press(navigation, Key.RIGHT, after=both, deadline_ns=deadline)
            right_up, deadline = self._change(lambda: keyboard.report(keyboard=(), consumer=0xE9), record, "right_up")
            released_right = self._release(navigation, after=right_up, deadline_ns=deadline)
            uncertainty = getattr(self.session._backends[navigation.gpio], "clock_uncertainty_ns", 0)
            if type(uncertainty) is not int or uncertainty < 0:
                raise AssertionError("BLE key wire clock uncertainty is unavailable")
            continuation = released_right.timestamp_ns + 2 * uncertainty
            continued = self._press(volume, Key.VOLUME_UP, after=continuation,
                                    deadline_ns=continuation + self._budget())
            final_up, deadline = self._change(keyboard.all_up, record, "all_up")
            final_release = self._release(volume, after=final_up, deadline_ns=deadline)
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

    def assert_shared_recipient(self, keyboard, component):
        """Two navigation holds share one CEC stream and one final release."""
        if component not in self.components:
            raise ValueError("The BLE key recipient is outside this scan")
        record = self._begin(keyboard, "shared_recipient")
        primary = None
        try:
            started, deadline = self._change(
                lambda: keyboard.report(keyboard=(0x4F,), consumer=0), record, "right_down")
            right = self._press(component, Key.RIGHT, after=started, deadline_ns=deadline)
            both, deadline = self._change(
                lambda: keyboard.report(keyboard=(0x4F, 0x52), consumer=0), record, "up_down")
            up = self._press(component, Key.UP, after=both, deadline_ns=deadline)
            right_up, _ = self._change(
                lambda: keyboard.report(keyboard=(0x52,), consumer=0), record, "right_up")
            selected, deadline = self._change(
                lambda: keyboard.report(keyboard=(0x28, 0x52), consumer=0), record, "select_barrier_down")
            barrier = self._press(component, Key.SELECT, after=selected, deadline_ns=deadline)
            select_up, deadline = self._change(
                lambda: keyboard.report(keyboard=(0x52,), consumer=0), record, "select_barrier_up")
            continued = self._press(component, Key.UP, after=select_up, deadline_ns=deadline)
            final_up, deadline = self._change(keyboard.all_up, record, "all_up")
            release = self._release(component, after=final_up, deadline_ns=deadline)
            self._drain(time.monotonic_ns())
            frames = [frame for gpio, frame in self._snapshot()
                      if gpio == component.gpio and frame.destination == component.address.logical
                      and started < frame.timestamp_ns]
            releases = [frame for frame in frames if frame.opcode == 0x45]
            if any(frame.timestamp_ns < final_up for frame in releases):
                raise AssertionError("A shared recipient was released before its last key ended")
            if len(releases) != 1:
                raise AssertionError("Shared keys must produce one final CEC release")
            # Select's new down follows Right-up through the ordered BLE and
            # owner delivery paths. Its physical frame is behind every Right
            # frame admitted before that up, in this recipient's CEC FIFO.
            if any(frame.opcode == 0x44 and frame.operands == bytes([Key.RIGHT])
                   and frame.timestamp_ns > barrier.timestamp_ns for frame in frames):
                raise AssertionError("Right restarted while only Up remained held")
            record.update(component=component.name,
                          right_press_wire_timestamp_ns=right.timestamp_ns,
                          up_press_wire_timestamp_ns=up.timestamp_ns,
                          right_up_monotonic_ns=right_up,
                          select_barrier_wire_timestamp_ns=barrier.timestamp_ns,
                          select_up_monotonic_ns=select_up,
                          continuing_up_wire_timestamp_ns=continued.timestamp_ns,
                          final_all_up_monotonic_ns=final_up,
                          release_frame=release.raw.hex(":"),
                          release_wire_timestamp_ns=release.timestamp_ns)
            return self._finish(record, routes={Key.RIGHT: component, Key.UP: component, Key.SELECT: component},
                                released=((component, release),), release_once=True)
        except BaseException as error:
            primary = error
            raise
        finally:
            self._all_up(keyboard, primary)

    def assert_failed_press(self, keyboard, component, *, after_repetition=False):
        """A physical destination NACK ends its BLE hold without replaying it."""
        from .nack import MissingCECRecipient, nack_attempts

        if component not in self.components:
            raise ValueError("The BLE key recipient is outside this scan")
        record = self._begin(keyboard, "failed_repeat" if after_repetition else "failed_press")
        primary = None
        try:
            # Member logs are not exposed by the shipping UI. On the main,
            # corroborate the partial physical frames with fresh per-frame
            # attempt diagnostics rather than attributing header-only NACKs
            # to opcodes that never reached the wire.
            logs_before = (self.dut.capture_logs(expected_boot=self.scan._boot)
                           if component.dut is self.dut else None)
            # Start capture before any hold. Withdrawal can NACK an in-flight
            # repeat after its header was ACKed; its start can precede the
            # withdrawal boundary without preceding this observation.
            with MissingCECRecipient(component, scan=self.scan, withdraw_on_enter=False) as failure:
                if after_repetition:
                    started, deadline = self._change(
                        lambda: keyboard.report(keyboard=(0x4F,), consumer=0), record, "right_down")
                    first = self._press(component, Key.RIGHT, after=started, deadline_ns=deadline)
                    repeated = self._press(component, Key.RIGHT, after=first.timestamp_ns,
                                           deadline_ns=first.timestamp_ns + self._budget())
                    record.update(first_press_wire_timestamp_ns=first.timestamp_ns,
                                  repeat_wire_timestamp_ns=repeated.timestamp_ns)
                failed_after = failure.withdraw()
                record["withdrawn_monotonic_ns"] = failed_after
                if not after_repetition:
                    failed_after, _ = self._change(
                        lambda: keyboard.report(keyboard=(0x4F,), consumer=0), record, "failed_right_down")
                initiator = self._initiators[component.gpio]

                def target_frames():
                    failure.assert_healthy()
                    return tuple(frame for frame in failure.frames
                                 if frame.source == initiator and frame.destination == component.address.logical)

                def failed_attempts(frames):
                    record["observed_failure_frames"] = [{"frame": frame.raw.hex(":"),
                        "wire_timestamp_ns": frame.timestamp_ns,
                        "eom": list(frame.eom), "ack": list(frame.ack)} for frame in frames]
                    attempts = nack_attempts(frames, (initiator << 4) | component.address.logical)
                    record.update(physical_nack_attempts=len(attempts),
                                  failed_wire_attempts=[{"frame": frame.raw.hex(":"),
                                      "wire_timestamp_ns": frame.timestamp_ns,
                                      "eom": list(frame.eom), "ack": list(frame.ack)} for frame in attempts])
                    if any(frame.opcode == 0x44 and frame.operands not in (b"", bytes([Key.RIGHT]))
                           for frame in attempts):
                        raise AssertionError("The physical failed press has the wrong key operand")
                    return attempts

                def first_failure(_):
                    attempts = failed_attempts(target_frames())
                    return min(attempts, key=lambda frame: frame.timestamp_ns) if attempts else None

                # A lone NACKed header is completed by the next real falling
                # edge. Drain with the physical witness before inspecting it.
                failure.drain(failed_after + self._budget())
                failed = self._wait(first_failure, deadline_ns=time.monotonic_ns() + self._budget(),
                                    message="No physical failed key transmission")
                failed_through = max(failed_after, failed.timestamp_ns) + self._budget()
                failure.drain(failed_through)
                frames = target_frames()
                attempts = failed_attempts(frames)
                # The failed press can be followed by a final release, whose
                # existing budget is three attempts. Some aborted frames have
                # no opcode; retain that ambiguity rather than invent one.
                if not 1 <= len(attempts) <= 4:
                    raise AssertionError("A failed BLE hold kept transmitting after its cleanup budget")
                if any(frame.opcode == 0x44 and frame.timestamp_ns > failed.timestamp_ns for frame in frames):
                    raise AssertionError("A failed BLE press continued repeating")

                failure.restore()
                restored = time.monotonic_ns()
                restored_through = restored + self._budget()
                failure.drain(restored_through)
                self._drain(restored_through)
                up, _ = self._change(keyboard.all_up, record, "failed_right_up")
                up_through = up + self._budget()
                failure.drain(up_through)
                self._drain(up_through)
                frames = target_frames()
                attempts = failed_attempts(frames)
                headers = [frame for frame in attempts if frame.opcode is None]
                if len(attempts) > 4:
                    raise AssertionError("A failed BLE hold kept transmitting after its cleanup budget")
                if any(frame.timestamp_ns > failed.timestamp_ns and frame.opcode == 0x44 for frame in frames):
                    raise AssertionError("A failed BLE hold restarted when its recipient returned")
                if any(frame.timestamp_ns > restored for frame in attempts):
                    raise AssertionError("A failed BLE hold submitted another destination attempt")
                if any(frame.timestamp_ns > up and frame.opcode == 0x45 for frame in frames):
                    raise AssertionError("The failed key's matching up was not a no-op")

                record.update(exact_press_attempts_verified=False,
                              attempt_evidence="physical_nacks_only")
                if logs_before is not None:
                    from .tx_policy import failed_key_attempts
                    logs_after = self.dut.capture_logs(expected_boot=self.scan._boot)
                    policy = failed_key_attempts(logs_before, logs_after,
                        gpio=component.dut.wiring.dut_cec_gpio(
                            component.dut.name, component.tentacle, component.direction),
                        header=(initiator << 4) | component.address.logical,
                        physical_attempts=len(attempts), evidence=record)
                    record.update(exact_press_attempts_verified=True,
                                  attempt_evidence="firmware_logs_and_physical_nacks",
                                  failed_transmissions=policy)

                fresh, deadline = self._change(
                    lambda: keyboard.report(keyboard=(0x4F,), consumer=0), record, "fresh_right_down")
                press = self._press(component, Key.RIGHT, after=fresh, deadline_ns=deadline)
                final_up, deadline = self._change(keyboard.all_up, record, "fresh_right_up")
                release = self._release(component, after=final_up, deadline_ns=deadline)
                record.update(component=component.name, failed_attempt_header=failed.raw[:1].hex(":"),
                              failed_attempt_frame=failed.raw.hex(":"),
                              failed_attempt_wire_timestamp_ns=failed.timestamp_ns,
                              failed_attempt_headers=[{"frame": frame.raw.hex(":"),
                                  "wire_timestamp_ns": frame.timestamp_ns,
                                  "eom": list(frame.eom), "ack": list(frame.ack)} for frame in headers],
                              failure_observed_through_monotonic_ns=failed_through,
                              restored_monotonic_ns=restored,
                              held_after_restore_through_monotonic_ns=restored_through,
                              failed_up_monotonic_ns=up,
                              failed_up_observed_through_monotonic_ns=up_through,
                              fresh_press_wire_timestamp_ns=press.timestamp_ns,
                              release_wire_timestamp_ns=release.timestamp_ns)
            def final_policy(frames):
                # Normal RX uses the last-byte timestamp, while pin frames
                # use the start edge. Inspect this late interval only after
                # restoration; the pin checks already cover the fault itself.
                target = [frame for gpio, frame in frames
                          if gpio == component.gpio and frame.destination == component.address.logical
                          and frame.timestamp_ns > restored]
                if any(frame.opcode == 0x44 and frame.timestamp_ns < fresh
                       for frame in target):
                    raise AssertionError("A failed BLE hold restarted before its fresh physical down")
                releases = [frame for frame in target if frame.opcode == 0x45]
                if [(frame.raw, frame.timestamp_ns) for frame in releases] != [(release.raw, release.timestamp_ns)]:
                    raise AssertionError("The failed key's matching up was not a no-op")
            return self._finish(record, routes={Key.RIGHT: component}, released=((component, release),),
                                validate_frames=final_policy)
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
            started, deadline = self._change(lambda: keyboard.report(keyboard=(), consumer=0xE9), record, "volume_down")
            first = self._press(component, Key.VOLUME_UP, after=started, deadline_ns=deadline)
            repeated = self._press(component, Key.VOLUME_UP, after=first.timestamp_ns,
                                   deadline_ns=first.timestamp_ns + self._budget())
            lost, deadline = self._change(keyboard.disconnect, record, "disconnect", kind="ble_disconnect")
            disconnected = True
            release = self._release(component, after=lost, deadline_ns=deadline)
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
