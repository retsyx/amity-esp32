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

from amity_harness import AvrComponent, CecSession, DUT, Frame, Hooks, PlaybackComponent, TestBed, TvComponent, TxResult
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
        self.closed.wait(timeout)
        return None

    def close(self):
        self.closed.set()


class Keyboard:
    def __init__(self, fixture):
        self.f = fixture
        self.volume = self.right = self.select = self.disconnected = False
        self.commands = []
        self.transport, self.stale, self.wrong_source = "wire", False, False
        self.coupled_release = self.missing_repeat = self.wrong_target = self.restart_right = False
        self.disconnect_release, self.disconnect_restart, self.ui_disconnected = True, False, True
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
        stamp = 1 if self.stale else time.monotonic_ns()
        source = 9 if self.wrong_source else self.f.scan.initiators[target.gpio]
        frame = Frame(source, target.address.logical, opcode, b"" if key is None else bytes([key]), stamp)
        for component in self.f.components:
            if component.gpio == target.gpio:
                component.receive(frame, transport=self.transport)

    def report(self, *, keyboard=(), consumer=0):
        self.commands.append((tuple(keyboard), consumer))
        if self.report_error is not None:
            error, self.report_error = self.report_error, None
            raise error
        if self.mutate is not None:
            mutate, self.mutate = self.mutate, None
            mutate()
        action = self.action("ble_report")
        volume, right, select = consumer == 0xE9, 0x4F in keyboard, 0x28 in keyboard
        if select and not self.select:
            if self.wrong_target:
                self.emit(self.f.tv, 0x44, 0)
            self.emit(self.f.player, 0x44, 0)
        if not select and self.select:
            self.emit(self.f.player, 0x45)
        if volume and not self.volume:
            self.emit(self.f.avr, 0x44, 0x41)
            self.emit(self.f.avr, 0x44, 0x41)
        if right and not self.right:
            self.emit(self.f.player, 0x44, 4)
        if not right and self.right:
            self.emit(self.f.player, 0x45)
            if self.restart_right:
                self.emit(self.f.player, 0x44, 4)
            if self.coupled_release:
                self.emit(self.f.avr, 0x45)
            if volume and not self.missing_repeat:
                self.emit(self.f.avr, 0x44, 0x41)
        if not volume and self.volume:
            self.emit(self.f.avr, 0x45)
        self.volume, self.right, self.select = volume, right, select
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
            yield fixture

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
                    f.session._wire_idle_since_ns = {}
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
