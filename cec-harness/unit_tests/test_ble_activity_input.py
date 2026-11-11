"""BLE activity handoffs require fresh wire commands and discard held input."""

from contextlib import contextmanager
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecSession, DUT, Frame, PlaybackComponent, PowerState, TvComponent
from amity_harness.activity import ActivityAction
from amity_harness.ble_activity_input import BLEActivityEvidence
from amity_harness.cec_pin import PinFrame
from amity_harness.keys import Key
from unit_tests.test_ble_input import KeyWire


class TransitionKeyboard:
    def __init__(self, fixture, fault=None):
        self.f, self.fault = fixture, fault
        self.keys, self.consumer, self.commands, self.timers = (), 0, [], []
        self.phase, self.selected, self.route_time = "initial", 0, 0
        self.committed_time = 0
        self.initialized_time = self.selection_return_time = 0
        self.selection_return_state = None
        self.readiness_started = threading.Event()
        self.commit_ready = threading.Event()
        self.fast_complete_event = threading.Event()
        self.fresh_probe_received = threading.Event()
        self.readiness_during_setup = False
        self.fallback_probes = threading.Event()
        self.expect_fallback = False
        self.selection_inputs = []
        self.select_down = False
        self.select_released = self.fast_complete = False
        self.selection_ownership = None
        self.setup_delay = .005
    def assert_paired(self):
        return self

    def emit(self, target, opcode, operands=b"", *, before_observe=None):
        destination = 15 if opcode == 0x86 and self.fault != "directed_route" else target.address.logical
        frame = Frame(self.f.scan.initiators[target.gpio], destination,
                      opcode, operands, timestamp_ns=time.monotonic_ns())
        repairing = self.f.old_source is self.f.next_player
        if ((repairing and target is self.f.next_player and opcode == 0x44 and operands == b"\x6d")
                or (not repairing and target is self.f.old_source and opcode == 0x36)):
            self.initialized_time = frame.timestamp_ns
        if before_observe is not None:
            before_observe()
        for component in self.f.components:
            if component.gpio == target.gpio:
                component.receive(frame, transport="wire")
        if self.f.pin_active and target.gpio == self.f.next_player.gpio:
            stamp = frame.timestamp_ns
            if opcode == 0x44 and operands == bytes([Key.SELECT]):
                if self.fault == "missing_pin":
                    return frame.timestamp_ns
                if self.fault == "completion_after_fallback":
                    stamp -= 75_000_000
            self.f.pin_frames.append(PinFrame.observed(frame.raw, timestamp_ns=stamp,
                eom=tuple(index == len(frame.raw) - 1 for index in range(len(frame.raw))),
                ack=(frame.destination == 15,) * len(frame.raw), synchronized_ns=self.f.pin_started))
        return frame.timestamp_ns

    def _route(self):
        f = self.f
        if self.expect_fallback:
            self.fallback_probes.wait(.04)
        if self.fault in ("late_local_commit", "slow_status_response"):
            # Give the independent status read a rendezvous while the wire
            # sequence is in flight; a serialized read misses this window.
            self.readiness_during_setup = self.readiness_started.wait(.025)
        if self.fault != "missing_power":
            self.emit(f.next_player, 0x44, b"\x6d")
            self.emit(f.next_player, 0x45)
        # Normal simulated commit happens when routing finishes, before its
        # observation wakes the host. The late-commit fault keeps these apart.
        self.phase = "gate"
        self.route_time = self.emit(f.avr, 0x86, f.next_player.address.physical.to_bytes(2, "big"),
                                   before_observe=None if self.fault == "late_local_commit" else self._commit)
        if self.fault == "late_local_commit":
            def commit_after_probe():
                if self.fresh_probe_received.wait(.5):
                    self._commit()

            timer = threading.Thread(target=commit_after_probe)
            self.timers.append(timer)
            timer.start()
        if self.fault == "old_replay":
            self.emit(f.old_source, 0x44, bytes([Key.RIGHT]))
            self.emit(f.old_source, 0x45)
        if self.fault == "volume_replay":
            self.emit(f.volume, 0x44, bytes([Key.VOLUME_UP]))
            self.emit(f.volume, 0x45)

    def _commit(self):
        f = self.f
        f.state.update(current_activity=0 if self.fault == "wrong_activity" else 1,
                       activity_settling=False, activity_error="")
        if self.fault == "configuration":
            f.state["expected_config_revision"] = "2"
        if self.fault == "boot":
            f.state["expected_boot_id"] = "c" * 32
        self.committed_time = time.monotonic_ns()
        self.commit_ready.set()

    def _accept_selection(self):
        self.selection_inputs.append("activity_selected")
        if self.fault == "front_before_back_release":
            self.emit(self.f.tv, 0x36)
        if self.fault not in ("missing_right_cancel", "release_only_on_right_up"):
            self.emit(self.f.old_source, 0x45)
        if self.f.volume is not None and self.fault not in ("missing_volume_cancel", "late_volume_cancel"):
            self.emit(self.f.volume, 0x45)
        if self.f.old_source is not self.f.next_player:
            if self.fault != "front_before_back_release":
                self.emit(self.f.tv, 0x36)
            self.emit(self.f.old_source, 0x36)
            self.emit(self.f.avr, 0x36)
        if self.fault == "late_volume_cancel":
            self.emit(self.f.volume, 0x45)
        self.f.state["activity_settling"] = True
        self.phase = "setup"

    def _setup_left(self):
        self.selection_inputs.append("setup_left")
        if self.fault == "setup_press":
            self.emit(self.f.next_player, 0x44, bytes([Key.LEFT]))
        timer = threading.Timer(self.setup_delay, self._route)
        self.timers.append(timer)
        timer.start()
        if self.fault == "late_setup_delivery":
            timer.join(timeout=.5)

    def _deliver_delayed_selection(self):
        self._accept_selection()
        self._setup_left()

    def report(self, *, keyboard=(), consumer=0):
        if consumer not in (0, 0xe9):
            raise AssertionError("Activity probes need the actual Volume Up usage")
        keyboard = tuple(keyboard)
        action = ActivityAction(self.f.main, self.f.session, time.monotonic_ns(), self.f.scan._boot,
                                dict(self.f.scan.initiators), "ble_report", "d" * 32, None,
                                state=deepcopy(self.preflight), scan=self.f.scan, testbed=self.f.bed,
                                owner_boots=dict(self.f.scan.owner_boots), owner_duts=dict(self.f.scan.owner_duts))
        self.commands.append((keyboard, consumer))
        previous, previous_consumer = self.keys, self.consumer
        self.keys, self.consumer = keyboard, consumer
        source = self.f.old_source if self.phase == "initial" else self.f.next_player
        if self.phase == "initial" and consumer == 0xe9 and previous_consumer == 0:
            self.emit(self.f.volume, 0x44, bytes([Key.VOLUME_UP]))
            self.emit(self.f.volume, 0x44, bytes([Key.VOLUME_UP]))
        if self.phase == "initial" and 0x4f in keyboard and 0x4f not in previous:
            self.emit(source, 0x44, bytes([Key.RIGHT]))
            self.emit(source, 0x44, bytes([Key.RIGHT]))
        if 0x3b in previous and 0x3b not in keyboard:
            activity = self.f.activity
            self.selection_ownership = (activity._needs_standby, activity._last_start_request,
                                        activity._last_start_terminal)
            if self.fault == "shortcut_delivery_error":
                raise OSError("BLE shortcut delivery outcome unknown")
            self.selected = action.started_ns
            if self.fault == "delayed_selection":
                self.phase = "selection_pending"
                timer = threading.Timer(.02, self._deliver_delayed_selection)
                self.timers.append(timer)
                timer.start()
                self.selection_return_state = deepcopy(self.f.state)
                self.selection_return_time = time.monotonic_ns()
            else:
                self._accept_selection()
        # The HID parser delivers shortcut-up before new key-down edges from
        # this same report. Left therefore enters the already accepted setup.
        if self.phase == "setup" and 0x50 in keyboard and 0x50 not in previous:
            self._setup_left()
        elif self.phase == "gate":
            if self.fault == "gated_up" and 0x52 in keyboard and 0x52 not in previous:
                self.emit(source, 0x44, bytes([Key.UP]))
            if self.fault == "gated_down" and 0x51 in keyboard and 0x51 not in previous:
                self.emit(source, 0x44, bytes([Key.DOWN]))
            if (not self.f.state["activity_settling"] and
                    ((not keyboard and consumer == 0) or time.monotonic_ns() > self.route_time + 1_000_000_000)):
                self.phase = "ready"
        if 0x51 in keyboard:
            self.fallback_probes.set()
        if self.fault == "release_only_on_right_up" and 0x4f in previous and 0x4f not in keyboard:
            self.emit(self.f.old_source, 0x45)
        if self.phase == "ready" and 0x28 in keyboard and 0x28 not in previous:
            if self.fault == "fallback_replay":
                self.emit(source, 0x44, bytes([Key.UP]))
            if self.fault == "volume_fallback_replay":
                self.emit(self.f.volume, 0x44, bytes([Key.VOLUME_UP]))
            if self.fault != "missing_fresh":
                if self.fault in ("late_tap_wire", "completion_after_fallback"):
                    timer = threading.Timer(max(0, (self.selected + 1_020_000_000 - time.monotonic_ns()) / 1e9),
                                            self.emit, args=(source, 0x44, bytes([Key.SELECT])))
                    self.timers.append(timer)
                    timer.start()
                else:
                    self.emit(source, 0x44, bytes([Key.SELECT]))
                    if self.fault == "select_repeated":
                        self.emit(source, 0x44, bytes([Key.SELECT]))
                self.select_down = True
                if self.fault in ("early_release", "early_release_repress"):
                    self.emit(source, 0x45)
                    if self.fault == "early_release_repress":
                        self.emit(source, 0x44, bytes([Key.SELECT]))
        if self.select_down and 0x28 not in keyboard:
            self.emit(source, 0x45)
            self.select_down = False
            self.select_released = True
        if self.fault == "late_local_commit" and 0x28 in keyboard:
            self.fresh_probe_received.set()
        if not keyboard and consumer == 0:
            for component in self.f.components:
                if component.pressed_key is not None:
                    self.emit(component, 0x45)
            if self.select_released:
                self.fast_complete = True
                self.fast_complete_event.set()
        return action

    def all_up(self):
        return self.report()

    @contextmanager
    def reports(self):
        self.preflight = deepcopy(self.f.state)
        try:
            yield self
        finally:
            self.all_up()


class BLEActivityEvidenceTests(unittest.TestCase):
    @contextmanager
    def fixture(self, *, repair=False, fault=None, remote=False):
        main = DUT("amity-dut")
        owner = DUT("amity-dut-2") if remote else main
        connection = owner.tentacle(0)
        old_power = PowerState.STANDBY if repair else PowerState.ON
        tv = TvComponent(connection.front, "Gate TV", power=old_power)
        avr = AvrComponent(connection.back, "Gate AVR", power=old_power)
        player = PlaybackComponent(avr, "Gate Player", port=1, power=old_power)
        next_player = PlaybackComponent(avr, "Gate Next", port=2, address_type="tuner",
                                        power=PowerState.ON if repair else PowerState.STANDBY)
        components = (tv, avr, player, next_player)
        with CecSession(components, backend_factory=KeyWire) as session:
            main._runtime = SimpleNamespace(cec_timeout=.12, timeout=1)
            duts = (main, owner) if remote else (main,)
            bed = SimpleNamespace(session=session, _entered=True, _closed=False, duts=duts)
            for dut in duts:
                dut._testbed, dut._runtime = bed, main._runtime
            boot = "b" * 32
            owner_boots = {dut.identity.device_id: boot if dut is main else "c" * 32 for dut in duts}
            owner_duts = {dut.identity.device_id: dut for dut in duts}
            scan = SimpleNamespace(dut=main, _session=session, _testbed=bed, expected=components,
                                   _boot=boot, owner_boots=owner_boots, owner_duts=owner_duts,
                                   initiators={tv.gpio: 4, avr.gpio: 0}, validate_owners=lambda components, **kwargs: None)
            main.last_scan = scan
            state = {"expected_boot_id": boot, "current_activity": 1 if repair else 0,
                     "activity_settling": False, "activity_error": "",
                     "expected_config_revision": "1", "expected_presentation_revision": "1"}
            main.get_system = lambda: {}
            main.get_state = lambda: deepcopy(state)
            main.get_configuration = lambda: {"activities": [{"activity_id": "1" * 32}, {"activity_id": "2" * 32}]}
            fixture = SimpleNamespace(main=main, tv=tv, avr=avr, player=player, next_player=next_player,
                                      volume=None if repair else avr,
                                      old_source=next_player if repair else player, components=components,
                                      session=session, bed=bed, scan=scan, state=state,
                                      activity=SimpleNamespace(activity_id="2" * 32, timeout=1, _needs_standby=False,
                                                               _last_start_request="previous-http-request",
                                                               _last_start_terminal=True),
                                      pin_active=False, pin_closed=False, pin_frames=[])
            fixture.keyboard = TransitionKeyboard(fixture, fault)
            fixture.status_reads = []
            fixture.read_status = lambda: {name: deepcopy(state[name]) for name in
                                           ("current_activity", "activity_settling", "activity_error")}

            def request(path, *args, **kwargs):
                self.assertEqual(path, "/api/status", "BLE handoffs may only read shipping readiness")
                self.assertEqual(args, ())
                fixture.status_reads.append(time.monotonic_ns())
                return fixture.read_status()

            main.request = request

            class PhysicalPins:
                def __init__(self, component, *, scan):
                    self.assert_source = component is next_player and scan is fixture.scan

                def __enter__(self):
                    if not self.assert_source or fixture.keyboard.commands:
                        raise AssertionError("Observe the selected source's wire before the HID sequence")
                    fixture.pin_active = True
                    fixture.pin_started = time.monotonic_ns()
                    return self

                def first_frame(self, raw, *, after, timeout):
                    if not fixture.pin_active or fixture.keyboard.select_down or next_player.pressed_key is not None:
                        raise AssertionError("Complete the ordinary final release before reading pin proof")
                    frames = [frame for frame in fixture.pin_frames if frame.raw == raw and frame.timestamp_ns > after]
                    if not frames:
                        raise TimeoutError("No original physical pin frame")
                    return min(frames, key=lambda frame: frame.timestamp_ns)

                def __exit__(self, *args):
                    fixture.pin_active, fixture.pin_closed = False, True

            try:
                with patch("amity_harness.ble_activity_input.PinObservation", PhysicalPins):
                    yield fixture
            finally:
                for timer in fixture.keyboard.timers:
                    timer.join(timeout=.5)

    def transition(self, fixture, *, all_up=True):
        fixture.keyboard.expect_fallback = not all_up
        with BLEActivityEvidence(fixture.main, fixture.scan, *fixture.components) as evidence:
            evidence.assert_transition(fixture.keyboard, old_source=fixture.old_source, source=fixture.next_player,
                                       display=None, old_display=None if fixture.old_source is fixture.next_player else fixture.tv,
                                       routing=fixture.avr, volume=fixture.volume,
                                       activity=fixture.activity, all_up=all_up)

    def test_source_change_and_same_target_repair_require_both_gate_exit_paths(self):
        for repair, all_up in ((False, False), (True, True)):
            with self.subTest(repair=repair, all_up=all_up), self.fixture(repair=repair) as fixture:
                self.transition(fixture, all_up=all_up)
                record, = fixture.bed.control_observations[-1]["checks"]
                self.assertTrue(record["passed"])
                self.assertEqual(record["target"], 1)
                self.assertLess(record["old_release_wire_timestamp_ns"], record["initialization_wire_timestamp_ns"])
                if not repair:
                    self.assertLess(record["first_volume_press_wire_timestamp_ns"], record["volume_repeat_wire_timestamp_ns"])
                    self.assertLess(record["volume_repeat_wire_timestamp_ns"], record["old_volume_release_wire_timestamp_ns"])
                    self.assertLess(record["old_volume_release_wire_timestamp_ns"], record["initialization_wire_timestamp_ns"])
                else:
                    self.assertIsNone(record["first_volume_press_wire_timestamp_ns"])
                    self.assertIsNone(record["volume_repeat_wire_timestamp_ns"])
                    self.assertIsNone(record["old_volume_release_wire_timestamp_ns"])
                self.assertLess(record["power_on_wire_timestamp_ns"], record["routing_wire_timestamp_ns"])
                changes = {change["phase"]: change for change in record["input_changes"]}
                partial = changes["partial_release"]["started_monotonic_ns"]
                self.assertLess(record["old_release_wire_timestamp_ns"], partial)
                self.assertLess(record["initialization_wire_timestamp_ns"], partial)
                if not repair:
                    self.assertLess(record["old_volume_release_wire_timestamp_ns"], partial)
                if all_up:
                    self.assertLess(record["routing_wire_timestamp_ns"], changes["gated_up_down"]["started_monotonic_ns"])
                else:
                    self.assertLess(changes["gated_down_down"]["finished_monotonic_ns"], record["routing_wire_timestamp_ns"])
                coalesced = next(change for change in record["input_changes"]
                                 if change["phase"] == "shortcut_up_setup_left_down")
                self.assertLess(coalesced["finished_monotonic_ns"], record["routing_wire_timestamp_ns"])
                if all_up:
                    self.assertLess(record["fresh_press_pin_start_ns"], record["fallback_lower_bound_monotonic_ns"])
                    self.assertEqual(record["fresh_press_pin_frame"], "03:44:00")
                    self.assertEqual(record["fresh_press_pin_eom"], [False, False, True])
                    self.assertEqual(record["fresh_press_pin_ack"], [False, False, False])
                    self.assertTrue(fixture.pin_closed)
                else:
                    self.assertGreater(record["fresh_press_wire_timestamp_ns"], record["fallback_lower_bound_monotonic_ns"])
                self.assertEqual(fixture.state["current_activity"], 1)
                self.assertEqual(fixture.keyboard.selection_ownership, (True, None, False))
                self.assertTrue(fixture.activity._needs_standby)
                self.assertIsNone(fixture.activity._last_start_request)
                self.assertTrue(fixture.activity._last_start_terminal)
                self.assertEqual(fixture.keyboard.selection_inputs, ["activity_selected", "setup_left"])
                consumer = 0 if repair else 0xe9
                shortcut = fixture.keyboard.commands.index(((0x4f, 0x3b), consumer))
                self.assertEqual(fixture.keyboard.commands[shortcut + 1], ((0x4f, 0x50), consumer))
                self.assertEqual(fixture.keyboard.commands[0], ((0x4f,), 0) if repair else ((), 0xe9))
                first_all_up = next(index for index, command in enumerate(fixture.keyboard.commands)
                                    if command == ((), 0))
                self.assertTrue(all(usage == consumer for _, usage in fixture.keyboard.commands[:first_all_up]))
                select_reports = [(keys, consumer) for keys, consumer in fixture.keyboard.commands if 0x28 in keys]
                self.assertTrue(all(usage == (0 if all_up else consumer) for _, usage in select_reports))
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                self.assertTrue(all(not component._wire_observers for component in fixture.components))
                self.assertTrue(all(component.pressed_key is None for component in fixture.components))

    def test_ble_selection_marks_pending_cleanup_before_delivery_can_fail(self):
        with self.fixture(fault="shortcut_delivery_error") as fixture:
            with self.assertRaisesRegex(OSError, "delivery outcome unknown"):
                self.transition(fixture)
            self.assertEqual(fixture.keyboard.selection_ownership, (True, None, False))
            self.assertTrue(fixture.activity._needs_standby)
            self.assertIsNone(fixture.activity._last_start_request)
            self.assertFalse(fixture.activity._last_start_terminal)
            self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
            self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_normal_right_up_cannot_supply_transition_cancellation(self):
        with self.fixture(fault="release_only_on_right_up") as fixture:
            with self.assertRaisesRegex(TimeoutError, "fresh physical BLE key release"):
                self.transition(fixture, all_up=False)
            check, = fixture.bed.control_observations[-1]["checks"]
            self.assertFalse(check["passed"])
            self.assertNotIn("partial_release", [change["phase"] for change in check["input_changes"]])
            self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
            self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_front_standby_can_precede_back_release_but_back_setup_cannot(self):
        with self.fixture(fault="front_before_back_release") as fixture:
            self.transition(fixture, all_up=False)
            record, = fixture.bed.control_observations[-1]["checks"]
            front = next(frame["wire_timestamp_ns"] for frame in record["frames"] if frame["frame"] == "40:36")
            self.assertLess(front, record["old_release_wire_timestamp_ns"])
            self.assertLess(front, record["old_volume_release_wire_timestamp_ns"])
            self.assertLess(record["old_release_wire_timestamp_ns"], record["initialization_wire_timestamp_ns"])
            self.assertLess(record["old_volume_release_wire_timestamp_ns"], record["initialization_wire_timestamp_ns"])
            self.assertTrue(record["passed"])
        with self.fixture(fault="late_volume_cancel") as fixture:
            with self.assertRaisesRegex(AssertionError, "stop both old holds before setup"):
                self.transition(fixture, all_up=False)

    def test_old_only_display_and_audio_need_fresh_standby_even_if_state_is_correct(self):
        for role in ("tv", "avr"):
            with self.subTest(role=role), self.fixture() as fixture:
                component = getattr(fixture, role)

                def drop_standby(receiver, frame):
                    if frame.opcode == 0x36:
                        receiver.set_power(PowerState.STANDBY)
                        return None
                    return frame

                component.hooks.before_rx = drop_standby
                with self.assertRaisesRegex(TimeoutError, "fresh DUT standby"):
                    self.transition(fixture)
                self.assertEqual(component.power, PowerState.STANDBY)
                self.assertTrue(fixture.keyboard.fast_complete)
                self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
                self.assertTrue(all(not item._wire_observers for item in fixture.components))

    def test_slow_ready_response_cannot_delay_the_physical_gate_and_all_up_burst(self):
        with self.fixture(fault="slow_status_response") as fixture:
            observed = []

            def status():
                if fixture.keyboard.selected:
                    started = time.monotonic_ns()
                    fixture.keyboard.readiness_started.set()
                    observed.append((started, fixture.keyboard.route_time, fixture.state["activity_settling"]))
                    self.assertTrue(fixture.keyboard.commit_ready.wait(.2),
                                    "The physical setup did not commit independently of its HTTP reply")
                    ready = deepcopy(fixture.state)
                    # A response can already contain ready state while its
                    # network delivery outlasts the one-second gate window.
                    # The host must deliver the complete physical burst first.
                    self.assertTrue(fixture.keyboard.fast_complete_event.wait(1.2),
                                    "The HTTP readiness response blocked the timed HID burst")
                    return ready
                return deepcopy(fixture.state)

            def narrow_status():
                state = status()
                return {name: state[name] for name in ("current_activity", "activity_settling", "activity_error")}

            fixture.read_status = narrow_status
            original_state = fixture.main.get_state

            def full_state():
                if fixture.keyboard.selected and not fixture.keyboard.fast_complete:
                    self.fail("A slow full editor read must not enter the timed transition burst")
                return original_state()

            fixture.main.get_state = full_state
            self.transition(fixture)
            record, = fixture.bed.control_observations[-1]["checks"]
            self.assertTrue(record["passed"])
            self.assertTrue(fixture.keyboard.readiness_during_setup)
            self.assertTrue(any(not route and settling for _, route, settling in observed))
            self.assertLess(observed[0][0], record["routing_wire_timestamp_ns"])
            self.assertLess(record["routing_wire_timestamp_ns"], fixture.keyboard.committed_time)
            first_probe = next(change["started_monotonic_ns"] for change in record["input_changes"]
                               if change["phase"] == "gated_up_down")
            self.assertLess(fixture.keyboard.committed_time, record["settled_monotonic_ns"])
            self.assertLess(record["final_release_wire_timestamp_ns"], record["settled_monotonic_ns"])
            self.assertLess(first_probe, record["fresh_press_wire_timestamp_ns"])
            self.assertLess(record["fresh_press_wire_timestamp_ns"], record["fallback_lower_bound_monotonic_ns"])
            self.assertTrue(fixture.activity._last_start_terminal)

    def test_a_fresh_probe_delivered_before_local_commit_cannot_claim_gate_success(self):
        with self.fixture(fault="late_local_commit") as fixture:
            def status():
                fixture.keyboard.readiness_started.set()
                if fixture.state["activity_settling"]:
                    time.sleep(.005)
                return {name: deepcopy(fixture.state[name]) for name in
                        ("current_activity", "activity_settling", "activity_error")}

            fixture.read_status = status
            with self.assertRaisesRegex(TimeoutError, "fresh physical BLE key press"):
                self.transition(fixture)
            record, = fixture.bed.control_observations[-1]["checks"]
            fresh = next(change for change in record["input_changes"]
                         if change["phase"] == "fresh_select_down")
            self.assertLess(fixture.keyboard.route_time, fresh["started_monotonic_ns"])
            self.assertLess(fresh["started_monotonic_ns"], fixture.keyboard.committed_time)
            self.assertFalse(record["passed"])
            self.assertFalse(any(frame.raw == b"\x03\x44\x00" for frame in fixture.pin_frames))
            self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
            self.assertTrue(fixture.pin_closed)
            self.assertTrue(all(component.pressed_key is None for component in fixture.components))

    def test_narrow_readiness_requires_typed_current_settling_and_error_fields(self):
        for field, wrong in (("current_activity", True), ("current_activity", "1"),
                             ("activity_settling", 0), ("activity_error", None)):
            with self.subTest(field=field, wrong=wrong), self.fixture(repair=True) as fixture:
                state = {"current_activity": 1, "activity_settling": False, "activity_error": ""}
                state[field] = wrong
                fixture.read_status = lambda: dict(state)
                with self.assertRaisesRegex(AssertionError, "invalid readiness state"):
                    self.transition(fixture)
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                self.assertTrue(all(not component._wire_observers for component in fixture.components))
        for missing in ("current_activity", "activity_settling", "activity_error"):
            with self.subTest(missing=missing), self.fixture(repair=True) as fixture:
                state = {"current_activity": 1, "activity_settling": False, "activity_error": ""}
                del state[missing]
                fixture.read_status = lambda: dict(state)
                with self.assertRaisesRegex(AssertionError, "invalid readiness state"):
                    self.transition(fixture)

    def test_narrow_readiness_does_not_replace_full_boot_and_configuration_fences(self):
        for changed in ("boot", "configuration"):
            with self.subTest(changed=changed), self.fixture(repair=True, fault=changed) as fixture:
                with self.assertRaises(AssertionError):
                    self.transition(fixture)
                self.assertTrue(fixture.status_reads)
                self.assertTrue(fixture.keyboard.fast_complete)
                self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
                self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_remote_all_up_requires_physical_press_before_the_main_fallback(self):
        with self.fixture(repair=True, fault="late_tap_wire", remote=True) as fixture:
            fixture.main._runtime.cec_timeout = 1.1
            with self.assertRaisesRegex(AssertionError, "All-up did not open"):
                self.transition(fixture)
            self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
            self.assertIsNone(fixture.next_player.pressed_key)
            self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
            self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_pin_start_proves_early_gate_admission_when_the_cec_frame_finishes_later(self):
        with self.fixture(repair=True, fault="completion_after_fallback", remote=True) as fixture:
            fixture.main._runtime.cec_timeout = 1.1
            self.transition(fixture)
            record, = fixture.bed.control_observations[-1]["checks"]
            self.assertLess(record["fresh_press_pin_start_ns"], record["fallback_lower_bound_monotonic_ns"])
            self.assertGreater(record["fresh_press_wire_timestamp_ns"], record["fallback_lower_bound_monotonic_ns"])
            self.assertTrue(record["passed"])
            self.assertTrue(fixture.pin_closed)

    def test_ordinary_tap_without_original_pin_evidence_cannot_prove_early_gate_admission(self):
        with self.fixture(repair=True, fault="missing_pin") as fixture:
            with self.assertRaisesRegex(TimeoutError, "original physical pin frame"):
                self.transition(fixture)
            self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
            self.assertIsNone(fixture.next_player.pressed_key)
            self.assertTrue(fixture.pin_closed)

    def test_ordered_owner_key_up_can_follow_two_select_presses_before_one_final_release(self):
        for remote in (False, True):
            with self.subTest(remote=remote), self.fixture(repair=True, fault="select_repeated", remote=remote) as fixture:
                self.transition(fixture)
                record, = fixture.bed.control_observations[-1]["checks"]
                changes = {change["phase"]: change for change in record["input_changes"]}
                frames = [frame for frame in record["frames"]
                          if frame["wire_timestamp_ns"] > changes["fresh_select_down"]["started_monotonic_ns"]]
                self.assertTrue(record["passed"])
                self.assertEqual(sum(frame["frame"] == "03:44:00" for frame in frames), 2)
                self.assertEqual(sum(frame["frame"] == "03:45" for frame in frames), 1)
                self.assertIsNone(fixture.next_player.pressed_key)
                self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_local_setup_can_exceed_one_second_before_a_timely_physical_gate_proof(self):
        with self.fixture(repair=True) as fixture:
            fixture.activity.timeout = 2
            fixture.keyboard.setup_delay = 1.02
            self.transition(fixture)
            record, = fixture.bed.control_observations[-1]["checks"]
            selected = next(change["started_monotonic_ns"] for change in record["input_changes"]
                            if change["phase"] == "shortcut_up_setup_left_down")
            self.assertGreater(record["routing_wire_timestamp_ns"] - selected, 1_000_000_000)
            self.assertLess(record["fresh_press_wire_timestamp_ns"], record["fallback_lower_bound_monotonic_ns"])
            self.assertTrue(record["passed"])

    def test_remote_handoff_uses_the_activity_budget_without_moving_the_gate_window(self):
        with self.fixture(remote=True) as fixture:
            fixture.activity.timeout = 2
            fixture.keyboard.setup_delay = 1.02
            self.transition(fixture, all_up=False)
            record, = fixture.bed.control_observations[-1]["checks"]
            changes = {change["phase"]: change for change in record["input_changes"]}
            selected = changes["shortcut_up_setup_left_down"]["started_monotonic_ns"]
            self.assertEqual(record["activity_deadline_monotonic_ns"], selected + 2_000_000_000)
            self.assertEqual(record["fallback_lower_bound_monotonic_ns"], selected + 1_000_000_000)
            self.assertLess(changes["gated_down_down"]["finished_monotonic_ns"], selected + 1_000_000_000)
            self.assertGreater(record["routing_wire_timestamp_ns"], selected + 1_000_000_000)
            self.assertLess(record["routing_wire_timestamp_ns"], record["activity_deadline_monotonic_ns"])
            self.assertLess(record["old_release_wire_timestamp_ns"], selected + 120_000_000)
            self.assertLess(record["old_volume_release_wire_timestamp_ns"], selected + 120_000_000)
            self.assertTrue(record["passed"])

    def test_initialization_and_readiness_share_the_original_activity_deadline(self):
        for delayed in ("route", "readiness"):
            with self.subTest(delayed=delayed), self.fixture(fault="delayed_selection", remote=True) as fixture:
                fixture.activity.timeout = .04
                if delayed == "route":
                    fixture.keyboard.setup_delay = .03
                else:
                    def delayed_status():
                        time.sleep(.08)
                        return {"current_activity": 1, "activity_settling": False, "activity_error": ""}

                    fixture.read_status = delayed_status
                with self.assertRaisesRegex(TimeoutError, "initialization command|did not become current"):
                    self.transition(fixture, all_up=False)
                record, = fixture.bed.control_observations[-1]["checks"]
                selected = next(change["started_monotonic_ns"] for change in record["input_changes"]
                                if change["phase"] == "shortcut_up_setup_left_down")
                self.assertEqual(record["activity_deadline_monotonic_ns"], selected + 40_000_000)
                self.assertFalse(record["passed"])
                self.assertNotIn("fresh_select_down", [change["phase"] for change in record["input_changes"]])

    def test_coalesced_setup_report_must_finish_before_final_routing(self):
        with self.fixture(fault="late_setup_delivery") as fixture:
            with self.assertRaisesRegex(AssertionError, "coalesced.*missed initialization"):
                self.transition(fixture)
            self.assertEqual(fixture.keyboard.selection_inputs, ["activity_selected", "setup_left"])
            self.assertGreater(fixture.keyboard.route_time, fixture.keyboard.selected)
            self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
            self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
            self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_same_target_readiness_waits_for_physical_acceptance_after_delayed_ble_delivery(self):
        with self.fixture(repair=True, fault="delayed_selection") as fixture:
            reads = []

            def status():
                if fixture.keyboard.selected:
                    self.assertGreater(fixture.keyboard.initialized_time, 0,
                                       "The old target-ready snapshot cannot prove the new selection")
                    reads.append(time.monotonic_ns())
                return deepcopy(fixture.state)

            fixture.main.get_state = status
            self.transition(fixture)
            record, = fixture.bed.control_observations[-1]["checks"]
            self.assertTrue(record["passed"])
            self.assertEqual(fixture.keyboard.selection_return_state["current_activity"], 1)
            self.assertFalse(fixture.keyboard.selection_return_state["activity_settling"])
            self.assertLess(fixture.keyboard.selection_return_time, record["initialization_wire_timestamp_ns"])
            self.assertLess(record["initialization_wire_timestamp_ns"], min(reads))
            self.assertGreater(record["settled_monotonic_ns"], fixture.keyboard.committed_time)
            self.assertTrue(fixture.activity._last_start_terminal)

    def test_raw_control_observation_does_not_wait_for_model_handling_lock(self):
        with self.fixture() as fixture:
            handling, release, observed = threading.Event(), threading.Event(), threading.Event()
            frames, failures = [], []
            original = Frame(fixture.scan.initiators[fixture.avr.gpio], 15, 0x86,
                             fixture.next_player.address.physical.to_bytes(2, "big"),
                             timestamp_ns=time.monotonic_ns())

            def pause_handling(component, frame):
                handling.set()
                release.wait(2)
                return frame

            fixture.avr.hooks.before_rx = pause_handling

            def receive():
                try:
                    with fixture.session._lock:
                        fixture.avr.receive(original, transport="wire")
                except BaseException as error:
                    failures.append(error)

            evidence = BLEActivityEvidence(fixture.main, fixture.scan, *fixture.components)
            # A raw helper observation alone must never count as a completed
            # activity proof; context exit still requires assert_transition.
            with self.assertRaisesRegex(RuntimeError, "not asserted"):
                with evidence:
                    original = Frame(original.source, original.destination, original.opcode, original.operands,
                                     timestamp_ns=time.monotonic_ns())

                    def inspect():
                        try:
                            frames.append(evidence._control(fixture.avr, 0x86, after=evidence._started,
                                          deadline_ns=original.timestamp_ns + 1_000_000_000,
                                          operands=original.operands))
                        except BaseException as error:
                            failures.append(error)
                        finally:
                            observed.set()

                    receiver, inspector = threading.Thread(target=receive), threading.Thread(target=inspect)
                    receiver.start()
                    try:
                        self.assertTrue(handling.wait(1))
                        inspector.start()
                        self.assertTrue(observed.wait(1), "Raw wire evidence waited for CEC model handling")
                        self.assertEqual(failures, [])
                        self.assertEqual(frames, [original])
                    finally:
                        release.set()
                        receiver.join(timeout=1)
                        if inspector.ident is not None:
                            inspector.join(timeout=1)
                    self.assertTrue(evidence._handled(fixture.avr, original))
            self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_captured_but_dropped_or_modified_controls_fail_after_the_fast_hid_burst(self):
        for kind in ("initialization", "power", "route"):
            for altered in ("dropped", "modified"):
                with self.subTest(kind=kind, altered=altered), self.fixture() as fixture:
                    component, opcode, operands = {
                        "initialization": (fixture.old_source, 0x36, b""),
                        "power": (fixture.next_player, 0x44, b"\x6d"),
                        "route": (fixture.avr, 0x86, fixture.next_player.address.physical.to_bytes(2, "big")),
                    }[kind]
                    captured = []

                    def intercept(receiver, frame):
                        if frame.opcode == opcode and frame.operands == operands:
                            captured.append(frame)
                            return None if altered == "dropped" else Frame(
                                frame.source, frame.destination, 0x9f, timestamp_ns=frame.timestamp_ns)
                        return frame

                    component.hooks.before_rx = intercept
                    with self.assertRaisesRegex(AssertionError, "captured.*dropped or modified"):
                        self.transition(fixture)
                    self.assertTrue(captured)
                    self.assertTrue(fixture.keyboard.fast_complete)
                    self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
                    self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                    self.assertTrue(all(not item._wire_observers for item in fixture.components))

    def test_old_setup_and_gated_press_leaks_cannot_hide_behind_correct_final_state(self):
        for fault in ("old_replay", "volume_replay", "setup_press", "gated_up", "gated_down",
                      "fallback_replay", "volume_fallback_replay"):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                with self.assertRaisesRegex(AssertionError, "old, setup or gated"):
                    self.transition(fixture, all_up=fault not in ("fallback_replay", "volume_fallback_replay"))
                self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_missing_power_fresh_press_early_release_or_wrong_ui_state_fail(self):
        for fault in ("missing_power", "missing_fresh", "early_release", "early_release_repress",
                      "wrong_activity", "configuration", "missing_right_cancel",
                      "missing_volume_cancel", "late_volume_cancel", "directed_route"):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                with self.assertRaises((AssertionError, TimeoutError)):
                    self.transition(fixture)
                self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                self.assertTrue(all(not component._wire_observers for component in fixture.components))


if __name__ == "__main__":
    unittest.main()
