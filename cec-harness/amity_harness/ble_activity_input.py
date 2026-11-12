"""Physical BLE activity handoffs and the all-buttons-up input gate."""

import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
from contextvars import copy_context

from .ble_input import BLEInputEvidence
from .components import PowerState
from .keys import Key
from .pin_observation import PinObservation
from .trace import event


class BLEActivityEvidence(BLEInputEvidence):
    def _validate(self):
        return super()._validate(allow_activity_change=True)

    def _live(self):
        # Original wire observers run before model replies. Keep BLE stimulus
        # independent of the responder lock while those replies transmit.
        if (not self._entered or self._closed or self.session._stop.is_set()
                or any(component._session is not self.session for component in self.components)):
            raise AssertionError("The CEC session ended during BLE activity evidence")

    def _observe(self, component, frame, transport):
        if transport != "wire" or frame.source != self._initiators.get(component.gpio):
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self._started:
                identity = component.gpio, frame.timestamp_ns, frame.raw
                if identity not in self._seen:
                    self._seen.add(identity)
                    self._frames.append((component.gpio, frame))
            self._condition.notify_all()

    def _control(self, component, opcode, *, after, deadline_ns, operands=None):
        def find(frames):
            matches = [frame for gpio, frame in frames
                       if gpio == component.gpio and frame.opcode == opcode
                       and frame.destination == (15 if opcode == 0x86 else component.address.logical)
                       and after < frame.timestamp_ns <= deadline_ns
                       and (operands is None or frame.operands == operands)]
            return min(matches, key=lambda frame: frame.timestamp_ns) if matches else None
        return self._wait(find, deadline_ns=deadline_ns,
                          message="No fresh physical activity initialization command")

    def _report_change(self, reports, record, phase, *, keyboard=(), consumer=0):
        before = time.monotonic_ns()
        action = reports.report(keyboard=keyboard, consumer=consumer)
        finished = time.monotonic_ns()
        if (getattr(action, "dut", None) is not self.dut
                or getattr(action, "session", None) is not self.session
                or getattr(action, "scan", None) is not self.scan
                or getattr(action, "testbed", None) is not self.bed
                or getattr(action, "boot", None) != self.scan._boot
                or getattr(action, "owner_boots", None) != self._owner_boots
                or getattr(action, "initiators", None) != self._initiators
                or type(getattr(action, "started_ns", None)) is not int
                or not before <= action.started_ns <= finished):
            raise AssertionError("The BLE transition report lost its physical action boundary")
        record.setdefault("input_changes", []).append({"phase": phase,
            "started_monotonic_ns": action.started_ns, "finished_monotonic_ns": finished})
        return action.started_ns, finished

    def _gate_probes(self, reports, record, consumer):
        self._report_change(reports, record, "gated_up_down", keyboard=(0x4F, 0x50, 0x52), consumer=consumer)
        self._report_change(reports, record, "partial_release", keyboard=(0x50, 0x52), consumer=consumer)
        _, finished = self._report_change(reports, record, "gated_down_down",
                                          keyboard=(0x50, 0x51, 0x52), consumer=consumer)
        return finished

    def _settled(self, target, deadline_ns):
        while True:
            remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
            if remaining <= 0:
                raise TimeoutError("The BLE-selected activity did not become current")
            state = self.dut.request("/api/status", timeout=min(15, remaining))
            if (not isinstance(state, dict) or type(state.get("current_activity")) is not int
                    or state["current_activity"] < -1 or type(state.get("activity_settling")) is not bool
                    or not isinstance(state.get("activity_error"), str)):
                raise AssertionError("The BLE activity handoff has invalid readiness state")
            observed = time.monotonic_ns()
            if observed >= deadline_ns:
                raise TimeoutError("The BLE-selected activity did not become current")
            if state.get("current_activity") == target and state.get("activity_settling") is False:
                if state.get("activity_error") != "":
                    raise AssertionError("The BLE activity handoff failed")
                return observed
            self._live()

    def assert_transition(self, keyboard, *, old_source, source, display, old_display, routing, volume, activity, all_up):
        """Cancel both old holds, discard setup input, then prove both gate exits.

        F1-F4 are the keyboard's actual activity shortcuts. One HID report
        releases that shortcut and presses Left. HID releases precede new
        downs in the input FIFO, placing Left behind the accepted selection.
        An all-up test must admit fresh input before the one-second fallback.
        """
        required = (old_source, source, routing) + tuple(component for component in (display, old_display, volume)
                                                        if component is not None)
        if any(component not in self.components for component in required):
            raise ValueError("BLE activity equipment must belong to the captured scan")
        rows = self.dut.get_configuration()["activities"]
        targets = [index for index, row in enumerate(rows) if row.get("activity_id") == activity.activity_id]
        if len(targets) != 1 or not 0 <= targets[0] <= 3:
            raise AssertionError("The BLE activity shortcut needs one of the first four saved activities")
        target = targets[0]
        record = self._begin(keyboard, "activity_all_up_gate" if all_up else "activity_gate_fallback")
        record.update(activity_id=activity.activity_id, target=target, owner=source.dut.identity.device_id)
        consumer = 0 if volume is None else 0xE9
        volume_first = volume_repeated = None
        with (ThreadPoolExecutor(max_workers=1) as readiness,
              PinObservation(source, scan=self.scan) if all_up else nullcontext() as pins,
              keyboard.reports() as reports):
            if volume is not None:
                volume_down, _ = self._report_change(reports, record, "old_volume_down", consumer=consumer)
                volume_first = self._press(volume, Key.VOLUME_UP, after=volume_down,
                                           deadline_ns=volume_down + self._budget())
                volume_repeated = self._press(volume, Key.VOLUME_UP, after=volume_first.timestamp_ns,
                                              deadline_ns=volume_first.timestamp_ns + self._budget())
            down, _ = self._report_change(reports, record, "old_right_down", keyboard=(0x4F,), consumer=consumer)
            first = self._press(old_source, Key.RIGHT, after=down, deadline_ns=down + self._budget())
            repeated = self._press(old_source, Key.RIGHT, after=first.timestamp_ns,
                                   deadline_ns=first.timestamp_ns + self._budget())
            self._report_change(reports, record, "shortcut_down", keyboard=(0x4F, 0x3A + target), consumer=consumer)
            # BLE can select this owned row without Activity.start(). Give its
            # normal cleanup the same pending-selection ownership beforehand.
            activity._needs_standby = True
            activity._last_start_request, activity._last_start_terminal = None, False
            selected, setup_sent = self._report_change(reports, record, "shortcut_up_setup_left_down",
                                                       keyboard=(0x4F, 0x50), consumer=consumer)
            # A handoff executes several ordered CEC actions. Give the whole
            # initialization its Activity budget, anchored at this selection;
            # cancellation, individual keys and the gate retain their bounds.
            activity_deadline = selected + int(activity.timeout * 1_000_000_000)
            record["activity_deadline_monotonic_ns"] = activity_deadline
            ended = self._release(old_source, after=selected, deadline_ns=selected + self._budget())
            volume_ended = (self._release(volume, after=selected, deadline_ns=selected + self._budget())
                            if volume is not None else ended)
            repairing = old_source is source
            initializer = source if repairing else old_source
            initialize_opcode = 0x44 if repairing else 0x36
            initialized = self._control(initializer, initialize_opcode,
                                        after=selected, deadline_ns=activity_deadline,
                                        operands=b"\x6d" if repairing else b"")
            # Cancellation and this initialization witness share the back
            # wire. Front-side display Standby is verified independently.
            if max(ended.timestamp_ns, volume_ended.timestamp_ns) >= initialized.timestamp_ns:
                raise AssertionError("The accepted BLE transition did not stop both old holds before setup")
            # A same-target selection initially has the old ready snapshot.
            # Start its readiness reads only after physical acceptance proof.
            settled_read = readiness.submit(copy_context().run, self._settled, target, activity_deadline)
            # Probe the held fallback during setup, after cancellation is
            # already physically proven while both old buttons remain down.
            blocked_sent = self._gate_probes(reports, record, consumer) if not all_up else None
            route = self._control(routing, 0x86, after=selected, deadline_ns=activity_deadline,
                                  operands=source.address.physical.to_bytes(2, "big"))
            powered = self._control(source, 0x44, after=selected, deadline_ns=activity_deadline,
                                    operands=b"\x6d")
            if powered.timestamp_ns >= route.timestamp_ns:
                raise AssertionError("The selected BLE source was not powered before routing")
            # One owner sends old-only Standby before new-only power-on.
            # Different owners run their sequences independently.
            if not repairing and initializer.dut is source.dut and initialized.timestamp_ns >= powered.timestamp_ns:
                raise AssertionError("The BLE handoff powered the new source before the old source's Standby")
            if setup_sent >= route.timestamp_ns:
                raise AssertionError("The coalesced BLE selection and setup report missed initialization")
            if all_up:
                # Routing bounds the earliest local fallback, not job commit.
                # Keep this burst independent of HTTP latency; only the fresh
                # physical pin start before fallback can prove early admission.
                blocked_sent = self._gate_probes(reports, record, consumer)
            else:
                # The fallback case has no fast all-up stimulus. Confirm setup
                # before observing the unchanged held inputs through expiry.
                settled = settled_read.result()
            # The main's input gate is independent of a member's CEC setup.
            # Submission precedes acceptance, giving a safe lower bound even
            # when the main finishes before a member's physical sequence.
            # Local setup cannot finish before its routing transmission. A
            # member's CEC sequence gives no such bound on the main's gate.
            fallback_lower = (route.timestamp_ns if source.dut is self.dut else selected) + 1_000_000_000
            fallback_observed = None
            if blocked_sent >= fallback_lower:
                raise AssertionError("The BLE gate probes missed the one-second release window")
            if all_up:
                self._report_change(reports, record, "all_up")
                held, held_consumer = (), 0
            else:
                # No held/swallowed button may replay when the fallback opens.
                fallback_observed = settled + 1_000_000_000 + self._budget()
                self._drain(fallback_observed)
                held, held_consumer = (0x50, 0x51, 0x52), consumer
            fresh_down, _ = self._report_change(reports, record, "fresh_select_down",
                                               keyboard=(*held, 0x28), consumer=held_consumer)
            fresh = self._press(source, Key.SELECT, after=fresh_down, deadline_ns=fresh_down + self._budget())
            fresh_up, _ = self._report_change(reports, record, "fresh_select_up", keyboard=held,
                                               consumer=held_consumer)
            released = self._release(source, after=fresh_up, deadline_ns=fresh_up + self._budget())
            if all_up:
                started = pins.first_frame(fresh.raw, after=fresh_down, timeout=self.dut._runtime.cec_timeout)
                if started.timestamp_ns >= fallback_lower:
                    raise AssertionError("All-up did not open the input gate before its fallback")
                record.update(fresh_press_pin_start_ns=started.timestamp_ns,
                              fresh_press_pin_frame=started.raw.hex(":"),
                              fresh_press_pin_eom=list(started.eom), fresh_press_pin_ack=list(started.ack))
            if (fresh.timestamp_ns >= released.timestamp_ns
                    or not self._handled(source, fresh) or not self._handled(source, released)):
                raise AssertionError("The fresh BLE tap did not produce its physical press and final release")
            self._report_change(reports, record, "final_all_up")
            if all_up:
                settled = settled_read.result()
            activity._last_start_terminal = True

        for component, frame in ((initializer, initialized),
                                 (source, powered), (routing, route)):
            if not self._handled(component, frame):
                raise AssertionError("The captured BLE activity command was dropped or modified")
        source.wait_for_power(PowerState.ON, since=selected, timeout=self.dut._runtime.cec_timeout)
        routing.wait_for_input(source, since=selected, timeout=self.dut._runtime.cec_timeout)
        if not repairing:
            for component in dict.fromkeys((old_display, old_source, volume)):
                if component is not None and component not in (display, source):
                    component.wait_for_power(PowerState.STANDBY, since=selected, command="standby",
                                             timeout=self.dut._runtime.cec_timeout)
        through = time.monotonic_ns() + self._budget()
        record["idle_receive_since_ns"] = self._drain(through)
        self._validate()
        through = time.monotonic_ns()
        record["idle_receive_since_ns"] = self._drain(through)
        frames = [(gpio, frame) for gpio, frame in self._snapshot()
                  if record["started_monotonic_ns"] < frame.timestamp_ns <= through]
        tap_frames = [frame for gpio, frame in frames if gpio == source.gpio
                      and frame.destination == source.address.logical and frame.timestamp_ns > fresh_down]
        if sum(frame.opcode == 0x45 for frame in tap_frames) != 1:
            raise AssertionError("The fresh BLE tap needs exactly one final release")
        for gpio, frame in frames:
            if (gpio == source.gpio and frame.destination == source.address.logical
                    and frame.opcode == 0x45 and fresh_down < frame.timestamp_ns < fresh_up):
                raise AssertionError("The fresh BLE key released while its physical button was still held")
            if frame.opcode != 0x44 or frame.operands == b"\x6d":
                continue
            old = (gpio == old_source.gpio and frame.destination == old_source.address.logical
                   and frame.operands == bytes([Key.RIGHT]) and frame.timestamp_ns < ended.timestamp_ns)
            old_volume = (volume is not None and gpio == volume.gpio and frame.destination == volume.address.logical
                          and frame.operands == bytes([Key.VOLUME_UP])
                          and frame.timestamp_ns < volume_ended.timestamp_ns)
            new = (gpio == source.gpio and frame.destination == source.address.logical
                   and frame.operands == bytes([Key.SELECT])
                   and fresh_down < frame.timestamp_ns < released.timestamp_ns)
            if not old and not old_volume and not new:
                raise AssertionError("An old, setup or gated BLE press crossed the accepted activity transition")
        if any(component.pressed_key is not None for component in self.components):
            raise AssertionError("The BLE activity handoff left a recipient pressed")
        record.update(first_press_wire_timestamp_ns=first.timestamp_ns,
                      repeat_wire_timestamp_ns=repeated.timestamp_ns,
                      old_release_wire_timestamp_ns=ended.timestamp_ns,
                      first_volume_press_wire_timestamp_ns=volume_first.timestamp_ns if volume_first else None,
                      volume_repeat_wire_timestamp_ns=volume_repeated.timestamp_ns if volume_repeated else None,
                      old_volume_release_wire_timestamp_ns=volume_ended.timestamp_ns if volume is not None else None,
                      initialization_wire_timestamp_ns=initialized.timestamp_ns,
                      power_on_wire_timestamp_ns=powered.timestamp_ns,
                      routing_wire_timestamp_ns=route.timestamp_ns,
                      fallback_lower_bound_monotonic_ns=fallback_lower,
                      settled_monotonic_ns=settled,
                      fallback_observation_end_monotonic_ns=fallback_observed,
                      fresh_press_wire_timestamp_ns=fresh.timestamp_ns,
                      final_release_wire_timestamp_ns=released.timestamp_ns,
                      observation_end_monotonic_ns=through,
                      frames=[{"gpio": gpio, "frame": frame.raw.hex(":"),
                               "wire_timestamp_ns": frame.timestamp_ns} for gpio, frame in frames], passed=True)
        event("ble_input.assert_passed", dut=self.dut.name, **record)
        return self
