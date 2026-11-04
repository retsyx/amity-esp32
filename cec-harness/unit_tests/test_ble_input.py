"""BLE keyboard reports must produce original, correctly routed CEC streams."""

from contextlib import contextmanager
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import json
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, CecSession, DUT, Frame, Hooks, PlaybackComponent,
                          PowerState, TestBed, TvComponent, TxResult)
from amity_harness.ble_input import BLEInputEvidence


class KeyWire:
    clock_uncertainty_ns = 0

    def __init__(self, gpio):
        self.closed = threading.Event()

    def open(self):
        pass

    def configure(self, components):
        pass

    def transmit(self, frame):
        return TxResult(frame, 1, time.monotonic_ns())

    def receive(self, timeout):
        # This fake has no queued RX; publish idle polls within the fixture's
        # short CEC deadline instead of racing its 50 ms production poll.
        self.closed.wait(min(timeout, .001))
        return None

    def close(self):
        self.closed.set()


class Keyboard:
    def __init__(self, fixture):
        self.f = fixture
        self.volume = self.right = self.up = self.select = self.disconnected = False
        self.commands = []
        self.transport, self.stale, self.wrong_source = "wire", False, False
        self.coupled_release = self.missing_repeat = self.wrong_target = self.restart_right = False
        self.disconnect_release, self.disconnect_restart, self.ui_disconnected = True, False, True
        self.shared_release = self.shared_missing_repeat = self.shared_restart_right = self.shared_double_release = False
        self.shared_select_release = self.shared_delayed_right_up = False
        self.failed = self.had_nack = False
        self.nack_headers = 4
        self.nack_defer_terminal = False
        self.nack_eom, self.nack_ack = (False,), (True,)
        self.nack_resume = self.nack_up_press = self.nack_up_release = self.nack_no_fresh = False
        self.report_error = None
        self.mutate = None
        self.preflight = self.delivery = None

    def action(self, kind):
        if self.preflight is not None:
            self.preflight()
        action = SimpleNamespace(started_ns=time.monotonic_ns(), dut=self.f.main,
            session=self.f.session, scan=self.f.scan, testbed=self.f.bed, kind=kind, boot=self.f.scan._boot,
            owner_boots=dict(self.f.scan.owner_boots), initiators=dict(self.f.scan.initiators), state=deepcopy(self.f.state))
        if self.delivery is not None:
            self.delivery()
        return action

    def assert_paired(self):
        if self.disconnected:
            raise AssertionError("Keyboard disconnected")

    def emit(self, target, opcode, key=None):
        if opcode == 0x44 and target.power == PowerState.OFF:
            if not self.failed:
                self.fail(target)
            return
        stamp = 1 if self.stale else time.monotonic_ns()
        source = 9 if self.wrong_source else self.f.scan.initiators[target.gpio]
        frame = Frame(source, target.address.logical, opcode, b"" if key is None else bytes([key]), stamp)
        monitor = getattr(self.f, "monitor", None)
        if monitor is not None and target.gpio == monitor.component.gpio and self.transport == "wire":
            monitor.observed.append(frame)
        for component in self.f.components:
            if component.gpio == target.gpio:
                component.receive(frame, transport=self.transport)

    def fail(self, target):
        self.failed = self.had_nack = True
        for index in range(self.nack_headers):
            frame = Frame(self.f.scan.initiators[target.gpio], target.address.logical,
                          timestamp_ns=time.monotonic_ns())
            observations = (self.f.monitor.pending if self.nack_defer_terminal and index == self.nack_headers - 1
                            else self.f.monitor.observed)
            observations.append(SimpleNamespace(**vars(frame), raw=frame.raw,
                                                eom=self.nack_eom, ack=self.nack_ack))

    def report(self, *, keyboard=(), consumer=0):
        self.commands.append((tuple(keyboard), consumer))
        if self.report_error is not None:
            error, self.report_error = self.report_error, None
            raise error
        if self.mutate is not None:
            mutate, self.mutate = self.mutate, None
            mutate()
        action = self.action("ble_report")
        volume, right, up, select = consumer == 0xE9, 0x4F in keyboard, 0x52 in keyboard, 0x28 in keyboard
        source_was_down = self.right or self.up or self.select
        if select and not self.select:
            if self.wrong_target:
                self.emit(self.f.tv, 0x44, 0)
            self.emit(self.f.player, 0x44, 0)
            if up and self.shared_restart_right:
                self.emit(self.f.player, 0x44, 4)
                self.emit(self.f.player, 0x44, 0)
        if not select and self.select:
            if up:
                if self.shared_select_release:
                    self.emit(self.f.player, 0x45)
                if not self.shared_missing_repeat:
                    self.emit(self.f.player, 0x44, 1)
        if volume and not self.volume:
            self.emit(self.f.avr, 0x44, 0x41)
            self.emit(self.f.avr, 0x44, 0x41)
        if right and not self.right:
            if not (self.had_nack and self.nack_no_fresh and self.f.player.power != PowerState.OFF):
                self.emit(self.f.player, 0x44, 4)
                self.emit(self.f.player, 0x44, 4)
        if up and not self.up:
            self.emit(self.f.player, 0x44, 1)
        if source_was_down and not (right or up or select) and not self.failed:
            self.emit(self.f.player, 0x45)
            if self.shared_double_release:
                self.emit(self.f.player, 0x45)
        if not right and self.right:
            if self.failed:
                if self.nack_up_press:
                    self.emit(self.f.player, 0x44, 4)
                if self.nack_up_release:
                    self.emit(self.f.player, 0x45)
            elif up and self.shared_release:
                self.emit(self.f.player, 0x45)
            if self.restart_right:
                self.emit(self.f.player, 0x44, 4)
            if self.coupled_release:
                self.emit(self.f.avr, 0x45)
            if volume and not self.missing_repeat:
                self.emit(self.f.avr, 0x44, 0x41)
            if up and not self.shared_missing_repeat:
                self.emit(self.f.player, 0x44, 1)
                self.emit(self.f.player, 0x44, 1)
            if up and self.shared_delayed_right_up:
                self.emit(self.f.player, 0x44, 4)
        if not volume and self.volume:
            self.emit(self.f.avr, 0x45)
        self.volume, self.right, self.up, self.select = volume, right, up, select
        if not right:
            self.failed = False
        return action

    def all_up(self):
        return self.report()

    def disconnect(self):
        self.commands.append(("disconnect",))
        action = self.action("ble_disconnect")
        self.disconnected = True
        if self.disconnect_release:
            self.emit(self.f.avr, 0x45)
        if self.disconnect_restart:
            self.emit(self.f.avr, 0x44, 0x41)
        return action

    def assert_disconnected(self):
        if not self.disconnected or not self.ui_disconnected:
            raise AssertionError("The UI did not confirm BLE link loss")


class BLEInputEvidenceTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        main = DUT("amity-dut")
        connection = main.tentacle(0)
        tv = TvComponent(connection.front, "Input TV")
        avr = AvrComponent(connection.back, "Input AVR")
        player = PlaybackComponent(avr, "Input Player", port=1)
        components = (tv, avr, player)
        with CecSession(components, backend_factory=KeyWire) as session:
            main._runtime = SimpleNamespace(cec_timeout=.012)
            bed = SimpleNamespace(session=session, _entered=True, _closed=False, duts=(main,))
            main._testbed = bed
            boot = "b" * 32
            scan = SimpleNamespace(dut=main, _session=session, _testbed=bed, expected=components,
                                   _boot=boot, owner_boots={main.identity.device_id: boot},
                                   owner_duts={main.identity.device_id: main},
                                   initiators={tv.gpio: 4, avr.gpio: 0}, validate_owners=lambda components: None)
            main.last_scan = scan
            state = {"expected_boot_id": boot, "current_activity": 0, "activity_settling": False,
                     "expected_config_revision": "1", "expected_presentation_revision": "1"}
            main.get_state = lambda: deepcopy(state)
            main.request = lambda *args, **kwargs: self.fail("BLE evidence must not issue HTTP key commands")
            for component in components:
                session._wire_idle_since_ns[component.gpio] = time.monotonic_ns() + 10**12
            fixture = SimpleNamespace(main=main, tv=tv, avr=avr, player=player, components=components,
                                      session=session, bed=bed, scan=scan, state=state)
            fixture.keyboard = Keyboard(fixture)
            scan.fixture = fixture
            yield fixture

    @contextmanager
    def failed_recipient(self, fixture):
        class MissingRecipient:
            def __init__(self, component, *, scan):
                if component is not fixture.player or scan is not fixture.scan:
                    raise AssertionError("The fault must target the selected physical recipient")
                self.component, self.previous = component, component.power
                self.observed, self.pending, self.restored = [], [], False
                fixture.failed_drains = self.drains = []

            def __enter__(self):
                fixture.monitor = self
                self.component.power_off()
                if fixture.keyboard.right:
                    fixture.keyboard.fail(self.component)
                return self

            @property
            def frames(self):
                return tuple(self.observed)

            def assert_healthy(self):
                if fixture.monitor is not self:
                    raise AssertionError("The physical fault observation ended")

            def drain(self, through):
                self.assert_healthy()
                self.drains.append(through)
                time.sleep(max(0, (through - time.monotonic_ns()) / 1e9))
                self.observed.extend(self.pending)
                self.pending.clear()

            def restore(self):
                if not self.restored:
                    self.component.set_power(self.previous)
                    self.restored = True
                    if fixture.keyboard.nack_resume:
                        fixture.keyboard.emit(self.component, 0x44, 4)

            def __exit__(self, *args):
                self.restore()
                fixture.monitor = None

        with patch.dict(sys.modules, {"amity_harness.nack": SimpleNamespace(MissingCECRecipient=MissingRecipient)}):
            yield

    def test_tap_overlap_and_disconnect_have_fresh_physical_proof_and_release_state(self):
        with self.fixture() as f:
            with BLEInputEvidence(f.main, f.scan, f.avr, f.player) as evidence:
                evidence.assert_tap(f.keyboard, f.player)
                evidence.assert_overlapping_buttons(f.keyboard, volume=f.avr, navigation=f.player)
                evidence.assert_disconnect_cleanup(f.keyboard, f.avr)
            checks = f.bed.control_observations[-1]["checks"]
            self.assertEqual([check["kind"] for check in checks], ["tap", "overlapping_buttons", "disconnect_cleanup"])
            self.assertTrue(all(check["passed"] and check["frames"] for check in checks))
            self.assertGreater(checks[1]["continuing_volume_press_wire_timestamp_ns"],
                               checks[1]["right_release_wire_timestamp_ns"])
            self.assertGreater(checks[2]["release_wire_timestamp_ns"], checks[2]["disconnected_monotonic_ns"])
            self.assertIsNone(f.avr.pressed_key)
            self.assertIsNone(f.player.pressed_key)
            self.assertEqual(f.keyboard.commands[-1], ("disconnect",))
            self.assertTrue(all(not component._wire_observers for component in f.components))

    def test_releasing_right_cannot_end_volume_then_restart_it(self):
        with self.fixture() as f:
            f.keyboard.coupled_release = True
            with self.assertRaisesRegex(AssertionError, "held Volume Up"):
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_overlapping_buttons(f.keyboard, volume=f.avr, navigation=f.player)
            self.assertEqual(f.keyboard.commands[-1], ((), 0))
            self.assertFalse(f.bed.control_observations[-1]["checks"][-1]["passed"])

    def test_shared_recipient_keeps_the_other_key_running_until_one_final_release(self):
        with self.fixture() as f:
            with BLEInputEvidence(f.main, f.scan) as evidence:
                evidence.assert_shared_recipient(f.keyboard, f.player)
            check, = f.bed.control_observations[-1]["checks"]
            self.assertEqual(check["kind"], "shared_recipient")
            self.assertTrue(check["passed"])
            self.assertGreater(check["continuing_up_wire_timestamp_ns"], check["right_up_monotonic_ns"])
            self.assertGreater(check["select_barrier_wire_timestamp_ns"], check["right_up_monotonic_ns"])
            self.assertGreater(check["continuing_up_wire_timestamp_ns"], check["select_up_monotonic_ns"])
            self.assertGreater(check["release_wire_timestamp_ns"], check["final_all_up_monotonic_ns"])
            self.assertEqual(sum(row["frame"].endswith(":45") for row in check["frames"]), 1)
            self.assertIsNone(f.player.pressed_key)
            self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_shared_recipient_requires_continuation_and_exactly_one_final_release(self):
        for failure, message in (("shared_release", "before its last key"),
                                 ("shared_select_release", "before its last key"),
                                 ("shared_double_release", "one final CEC release"),
                                 ("shared_missing_repeat", "fresh physical BLE key press"),
                                 ("shared_restart_right", "Right restarted")):
            with self.subTest(failure=failure), self.fixture() as f:
                setattr(f.keyboard, failure, True)
                with self.assertRaisesRegex((AssertionError, TimeoutError), message):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_shared_recipient(f.keyboard, f.player)
                self.assertFalse(f.bed.control_observations[-1]["checks"][-1]["passed"])
                self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_member_up_delivery_may_lag_two_up_frames_but_must_precede_select_barrier(self):
        with self.fixture() as f:
            f.keyboard.shared_delayed_right_up = True
            with BLEInputEvidence(f.main, f.scan) as evidence:
                evidence.assert_shared_recipient(f.keyboard, f.player)
            check, = f.bed.control_observations[-1]["checks"]
            before_barrier = [row for row in check["frames"]
                              if check["right_up_monotonic_ns"] < row["wire_timestamp_ns"]
                              < check["select_barrier_wire_timestamp_ns"]]
            self.assertEqual([row["frame"].split(":")[-1] for row in before_barrier], ["01", "01", "04"])
            self.assertTrue(check["passed"])
            self.assertEqual(sum(row["frame"].endswith(":45") for row in check["frames"]), 1)

    def test_first_or_repeated_press_nack_stops_the_hold_and_allows_fresh_input(self):
        for after_repetition in (False, True):
            with self.subTest(after_repetition=after_repetition), self.fixture() as f, self.failed_recipient(f):
                original_power = f.player.power
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_failed_press(f.keyboard, f.player, after_repetition=after_repetition)
                check, = f.bed.control_observations[-1]["checks"]
                self.assertEqual(check["kind"], "failed_repeat" if after_repetition else "failed_press")
                self.assertEqual(len(check["failed_attempt_headers"]), 4)
                self.assertTrue(all(row["eom"] == [False] and row["ack"] == [True]
                                    for row in check["failed_attempt_headers"]))
                self.assertTrue(check["passed"] and check["failed_attempts_include_release_cleanup"])
                self.assertGreater(check["fresh_press_wire_timestamp_ns"], check["failed_up_observed_through_monotonic_ns"])
                if after_repetition:
                    self.assertGreater(check["repeat_wire_timestamp_ns"], check["first_press_wire_timestamp_ns"])
                    self.assertGreater(check["failed_attempt_wire_timestamp_ns"], check["repeat_wire_timestamp_ns"])
                self.assertEqual(f.player.power, original_power)
                self.assertIsNone(f.player.pressed_key)
                self.assertIsNone(f.monitor)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_failure_needs_an_actual_attempt_and_no_continuation_or_matching_up_effect(self):
        faults = (("nack_headers", 0, "physical failed key transmission"),
                  ("nack_eom", (True,), "non-poll destination NACK"),
                  ("nack_ack", (False,), "non-poll destination NACK"),
                  ("nack_headers", 5, "cleanup budget"),
                  ("nack_resume", True, "restarted"),
                  ("nack_up_press", True, "restarted"),
                  ("nack_up_release", True, "not a no-op"),
                  ("nack_no_fresh", True, "fresh physical BLE key press"))
        for name, value, message in faults:
            with self.subTest(fault=name), self.fixture() as f, self.failed_recipient(f):
                original_power = f.player.power
                setattr(f.keyboard, name, value)
                with self.assertRaisesRegex((AssertionError, TimeoutError), message):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_failed_press(f.keyboard, f.player)
                self.assertFalse(f.bed.control_observations[-1]["checks"][-1]["passed"])
                self.assertEqual(f.player.power, original_power)
                self.assertIsNone(f.monitor)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_single_nack_without_release_is_completed_by_initial_physical_drain(self):
        for after_repetition in (False, True):
            with self.subTest(after_repetition=after_repetition), self.fixture() as f, self.failed_recipient(f):
                f.keyboard.nack_headers = 1
                f.keyboard.nack_defer_terminal = True
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_failed_press(f.keyboard, f.player, after_repetition=after_repetition)
                check, = f.bed.control_observations[-1]["checks"]
                self.assertTrue(check["passed"])
                self.assertEqual(len(check["failed_attempt_headers"]), 1)
                self.assertEqual(len(f.failed_drains), 4)
                self.assertLess(f.failed_drains[0], f.failed_drains[1])
                self.assertEqual(f.failed_drains[1], check["failed_attempt_wire_timestamp_ns"]
                                 + int(f.main._runtime.cec_timeout * 1e9))
                if not after_repetition:
                    down = next(change for change in check["input_changes"] if change["phase"] == "failed_right_down")
                    self.assertEqual(f.failed_drains[0], down["deadline_monotonic_ns"])
                self.assertIsNone(f.player.pressed_key)
                self.assertIsNone(f.monitor)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_missing_continuation_or_right_restart_cannot_pass_overlap(self):
        for failure in ("missing_repeat", "restart_right"):
            with self.subTest(failure=failure), self.fixture() as f:
                setattr(f.keyboard, failure, True)
                with self.assertRaises((AssertionError, TimeoutError)):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_overlapping_buttons(f.keyboard, volume=f.avr, navigation=f.player)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_virtual_stale_foreign_dropped_and_rewritten_input_do_not_prove_tap(self):
        for failure in ("virtual", "stale", "foreign", "dropped", "rewritten"):
            with self.subTest(failure=failure), self.fixture() as f:
                if failure == "virtual":
                    f.keyboard.transport = "virtual"
                elif failure == "stale":
                    f.keyboard.stale = True
                elif failure == "foreign":
                    f.keyboard.wrong_source = True
                elif failure == "dropped":
                    f.player.hooks = Hooks(before_rx=lambda component, frame: None)
                else:
                    f.player.hooks = Hooks(before_rx=lambda component, frame:
                        Frame(frame.source, frame.destination, frame.opcode, b"\x01", frame.timestamp_ns))
                with self.assertRaises((AssertionError, TimeoutError)):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_tap(f.keyboard, f.player)

    def test_extra_wrong_recipient_press_cannot_hide_behind_correct_press_or_hook(self):
        with self.fixture() as f:
            f.keyboard.wrong_target = True
            f.tv.hooks = Hooks(before_rx=lambda component, frame: None)
            with self.assertRaisesRegex(AssertionError, "wrong physical recipient"):
                with BLEInputEvidence(f.main, f.scan, f.player) as evidence:
                    evidence.assert_tap(f.keyboard, f.player)

    def test_boot_configuration_activity_and_scan_fences_survive_input(self):
        for failure in ("boot", "config", "activity", "owner", "scan"):
            with self.subTest(failure=failure), self.fixture() as f:
                def mutate():
                    if failure == "boot":
                        f.state["expected_boot_id"] = "c" * 32
                    elif failure == "config":
                        f.state["expected_config_revision"] = "2"
                    elif failure == "activity":
                        f.state["current_activity"] = 1
                    elif failure == "owner":
                        f.scan.owner_boots[f.main.identity.device_id] = "c" * 32
                    else:
                        f.main.last_scan = None
                f.keyboard.mutate = mutate
                with self.assertRaises(AssertionError):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_tap(f.keyboard, f.player)

    def test_disconnect_requires_physical_release_no_restart_and_shipping_ui_confirmation(self):
        for failure in ("disconnect_release", "disconnect_restart", "ui_disconnected"):
            with self.subTest(failure=failure), self.fixture() as f:
                setattr(f.keyboard, failure, failure == "disconnect_restart")
                with self.assertRaises((AssertionError, TimeoutError)):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_disconnect_cleanup(f.keyboard, f.avr)
                self.assertEqual(f.keyboard.commands[-1], ("disconnect",))

    def test_missing_receive_coverage_and_report_failure_both_release_keyboard(self):
        for failure in ("coverage", "report"):
            with self.subTest(failure=failure), self.fixture() as f:
                if failure == "coverage":
                    class Undrained(dict):
                        def __setitem__(self, key, value):
                            pass
                    f.session._wire_idle_since_ns = Undrained()
                else:
                    f.keyboard.report_error = OSError("Report transport failed")
                with self.assertRaises((OSError, TimeoutError)):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_tap(f.keyboard, f.player)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_slow_ui_preflight_does_not_consume_each_input_changes_cec_budget(self):
        # Advance only the monotonic clock: each UI preflight takes six seconds
        # without making this host regression wait on a real network or clock.
        real_clock, offset = time.monotonic_ns, [0]
        with self.fixture() as f:
            f.main._runtime.cec_timeout = 5
            f.keyboard.preflight = lambda: offset.__setitem__(0, offset[0] + 6_000_000_000)
            with patch("amity_harness.ble_input.time.monotonic_ns", side_effect=lambda: real_clock() + offset[0]):
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_tap(f.keyboard, f.player)
                    evidence.assert_overlapping_buttons(f.keyboard, volume=f.avr, navigation=f.player)
            tap, overlap = f.bed.control_observations[-1]["checks"]
            self.assertTrue(tap["passed"] and overlap["passed"])
            self.assertGreater(tap["input_changes"][0]["started_monotonic_ns"] - tap["started_monotonic_ns"], 5_000_000_000)
            self.assertGreater(overlap["continuing_volume_press_wire_timestamp_ns"]
                               - overlap["first_volume_press_wire_timestamp_ns"], 5_000_000_000)
            for check in (tap, overlap):
                self.assertTrue(all(change["deadline_monotonic_ns"] - change["started_monotonic_ns"] == 5_000_000_000
                                    for change in check["input_changes"]))

    def test_cec_response_after_five_seconds_from_actual_report_still_fails(self):
        real_clock, offset = time.monotonic_ns, [0]
        with self.fixture() as f:
            f.main._runtime.cec_timeout = 5
            f.keyboard.delivery = lambda: offset.__setitem__(0, offset[0] + 6_000_000_000)
            with patch("amity_harness.ble_input.time.monotonic_ns", side_effect=lambda: real_clock() + offset[0]):
                with self.assertRaisesRegex(TimeoutError, "fresh physical BLE key press"):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_tap(f.keyboard, f.player)
            self.assertFalse(f.bed.control_observations[-1]["checks"][-1]["passed"])
            self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_report_retains_proof_but_drops_private_top_level_fields(self):
        with self.fixture() as f:
            with BLEInputEvidence(f.main, f.scan) as evidence:
                evidence.assert_tap(f.keyboard, f.player)
            report_bed = TestBed(f.tv)
            report_bed.control_observations = f.bed.control_observations
            report_bed.control_observations[0]["private_token"] = "PRIVATE_BLE_TOKEN"
            report = report_bed.report
            self.assertTrue(report["control_observations"][0]["checks"][0]["passed"])
            self.assertNotIn("PRIVATE_BLE_TOKEN", json.dumps(report))

    def test_unasserted_context_is_an_error_and_detaches_observers(self):
        with self.fixture() as f:
            with self.assertRaisesRegex(RuntimeError, "not asserted"):
                with BLEInputEvidence(f.main, f.scan):
                    pass
            self.assertTrue(all(not component._wire_observers for component in f.components))


if __name__ == "__main__":
    unittest.main()
