"""Generic BLE tap/hold evidence preserves HID timing and physical assertions."""

from contextlib import contextmanager
from types import SimpleNamespace
import threading
import time
import unittest
from unittest.mock import patch

from amity_harness.ble_input import BLEInputEvidence
from amity_harness.keys import Key
from unit_tests import test_ble_input as input_fixture


class ReportKeyboard(input_fixture.Keyboard):
    """Physical-frame fixture with one preflight per real-report group."""

    def __init__(self, fixture):
        super().__init__(fixture)
        self.contexts = self.cleanup_reports = 0
        self.preflight_delay = self.press_delay = 0
        self.missing_release = self.early_release = self.bad_release = False
        self.exit_change = None
        self.in_reports = False
        self.down = None
        self.down_ns = self.up_ns = self.first_press_ns = None
        self.timers = []

    @contextmanager
    def reports(self):
        self.assert_paired()
        self.contexts += 1
        time.sleep(self.preflight_delay)
        self.in_reports = True
        try:
            yield self
        finally:
            self.cleanup_reports += 1
            self.all_up()
            self.in_reports = False
            if self.exit_change is not None:
                self.exit_change()

    def report(self, *, keyboard=(), consumer=0):
        if not self.in_reports:
            raise AssertionError("Timed reports must stay inside their preflight group")
        self.commands.append((tuple(keyboard), consumer))
        if self.report_error is not None:
            error, self.report_error = self.report_error, None
            raise error
        action = self.action("ble_report")
        if keyboard or consumer:
            if not self.commands[:-1] or self.commands[-2] != ((), 0):
                raise AssertionError("A fresh all-up report must clear the activity input gate")
            key = {0x4F: Key.RIGHT, 0x50: Key.LEFT}.get(keyboard[0]) if keyboard else {
                0xE9: Key.VOLUME_UP, 0xEA: Key.VOLUME_DOWN}[consumer]
            target = self.f.avr if consumer else self.f.player
            if self.wrong_target:
                target = self.f.tv
            self.down = target, key
            self.down_ns = action.started_ns

            def press():
                self.first_press_ns = time.monotonic_ns()
                self.emit(target, 0x44, key)
                if self.early_release:
                    self.emit(target, 0x45)

            if self.press_delay:
                timer = threading.Timer(self.press_delay, press)
                self.timers.append(timer)
                timer.start()
            else:
                press()
        elif self.down is not None:
            target, _ = self.down
            self.down = None
            self.up_ns = action.started_ns

            def release():
                if not self.missing_release:
                    self.emit(target, 0x45, 1 if self.bad_release else None)

            if self.press_delay and self.first_press_ns is None:
                timer = threading.Timer(self.press_delay, release)
                self.timers.append(timer)
                timer.start()
            else:
                release()
        return action

    def finish(self):
        for timer in self.timers:
            timer.join(timeout=.5)


class BLEKeyTests(unittest.TestCase):
    fixture = input_fixture.BLEInputEvidenceTests.fixture

    @contextmanager
    def reports(self):
        with self.fixture() as fixture:
            fixture.keyboard = ReportKeyboard(fixture)
            try:
                yield fixture
            finally:
                fixture.keyboard.finish()

    def test_navigation_and_consumer_taps_and_holds_prove_physical_press_and_final_release(self):
        with self.reports() as f:
            f.keyboard.preflight_delay = .015
            with BLEInputEvidence(f.main, f.scan) as evidence:
                for target, key, usage, consumer, duration in (
                        (f.player, Key.RIGHT, 0x4F, None, 0),
                        (f.avr, Key.VOLUME_UP, None, 0xE9, 750),
                        (f.player, Key.LEFT, 0x50, None, 650),
                        (f.avr, Key.VOLUME_DOWN, None, 0xEA, 0)):
                    evidence.assert_key(f.keyboard, target, key=key, keyboard_usage=usage,
                                        consumer_usage=consumer, duration_ms=duration)
            checks = f.bed.control_observations[-1]["checks"]
            self.assertEqual([check["key"] for check in checks], [4, 0x41, 3, 0x42])
            self.assertEqual(f.keyboard.contexts, 4)
            self.assertEqual(f.keyboard.cleanup_reports, 4)
            for check in checks:
                self.assertTrue(check["passed"])
                self.assertGreater(check["down_monotonic_ns"], check["gate_all_up_monotonic_ns"])
                self.assertGreaterEqual(check["all_up_monotonic_ns"] - check["down_monotonic_ns"],
                                        check["duration_ms"] * 1_000_000)
                self.assertGreater(check["release_wire_timestamp_ns"], check["all_up_monotonic_ns"])
                self.assertEqual(check["presses"], 1)
                self.assertEqual(check["release_frame"].split(":")[1:], ["45"])
            self.assertEqual(f.keyboard.commands[-1], ((), 0))
            self.assertTrue(all(component.pressed_key is None for component in f.components))

    def test_hold_up_is_not_delayed_by_waiting_for_its_first_physical_press(self):
        with self.reports() as f:
            f.main._runtime = SimpleNamespace(cec_timeout=.2)
            f.keyboard.press_delay = .06
            f.keyboard.preflight_delay = .03
            with BLEInputEvidence(f.main, f.scan) as evidence:
                evidence.assert_key(f.keyboard, f.player, key=Key.LEFT,
                                    keyboard_usage=0x50, duration_ms=20)
            check, = f.bed.control_observations[-1]["checks"]
            self.assertLess(check["all_up_monotonic_ns"], check["press_wire_timestamp_ns"])
            self.assertGreaterEqual(check["all_up_monotonic_ns"] - check["down_monotonic_ns"], 20_000_000)
            self.assertTrue(check["passed"])

    def test_timed_hold_does_not_take_the_cec_responder_lock_before_hid_up(self):
        with self.reports() as f:
            healthy = f.session.assert_healthy

            def checked_health():
                self.assertIsNone(f.keyboard.down, "CEC lock reads must follow the timed HID up")
                return healthy()

            with patch.object(f.session, "assert_healthy", side_effect=checked_health):
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_key(f.keyboard, f.avr, key=Key.VOLUME_UP,
                                        consumer_usage=0xE9, duration_ms=5)

    def test_virtual_stale_or_wrong_recipient_press_cannot_qualify_and_hid_is_released(self):
        for fault, value in (("transport", "virtual"), ("stale", True), ("wrong_target", True)):
            with self.subTest(fault=fault), self.reports() as f:
                setattr(f.keyboard, fault, value)
                with self.assertRaises((AssertionError, TimeoutError)):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_key(f.keyboard, f.player, key=Key.RIGHT, keyboard_usage=0x4F)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))
                self.assertEqual(f.keyboard.cleanup_reports, 1)
                self.assertFalse(f.bed.control_observations[-1]["checks"][-1]["passed"])

    def test_missing_operandful_or_early_release_cannot_pass(self):
        for fault in ("missing_release", "bad_release", "early_release"):
            with self.subTest(fault=fault), self.reports() as f:
                setattr(f.keyboard, fault, True)
                with self.assertRaises((AssertionError, TimeoutError)):
                    with BLEInputEvidence(f.main, f.scan) as evidence:
                        evidence.assert_key(f.keyboard, f.avr, key=Key.VOLUME_UP,
                                            consumer_usage=0xE9, duration_ms=5)
                self.assertEqual(f.keyboard.commands[-1], ((), 0))
                self.assertEqual(f.keyboard.cleanup_reports, 1)
                self.assertFalse(f.bed.control_observations[-1]["checks"][-1]["passed"])

    def test_failure_is_propagated_without_http_key_fallback_and_state_changes_remain_fenced(self):
        with self.reports() as f:
            f.keyboard.report_error = RuntimeError("HID submission failed")
            with self.assertRaisesRegex(RuntimeError, "HID submission failed"):
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_key(f.keyboard, f.player, key=Key.RIGHT, keyboard_usage=0x4F)
            self.assertEqual(f.keyboard.commands, [((), 0), ((), 0)])
            self.assertEqual(f.keyboard.cleanup_reports, 1)
        with self.reports() as f:
            f.keyboard.exit_change = lambda: f.state.update(expected_config_revision="2")
            with self.assertRaisesRegex(AssertionError, "activity, configuration or input owner changed"):
                with BLEInputEvidence(f.main, f.scan) as evidence:
                    evidence.assert_key(f.keyboard, f.player, key=Key.RIGHT, keyboard_usage=0x4F)
            self.assertEqual(f.keyboard.commands[-1], ((), 0))

    def test_invalid_usages_keys_or_durations_are_rejected_before_hid(self):
        with self.reports() as f:
            evidence = BLEInputEvidence(f.main, f.scan)
            for changes in ({}, {"keyboard_usage": 0x4F, "consumer_usage": 0xE9},
                            {"keyboard_usage": True}, {"keyboard_usage": 3},
                            {"keyboard_usage": 0xE8}, {"consumer_usage": 0},
                            {"consumer_usage": 0x10000}, {"consumer_usage": True},
                            {"keyboard_usage": 0x4F, "duration_ms": -1},
                            {"keyboard_usage": 0x4F, "duration_ms": True},
                            {"keyboard_usage": 0x4F, "duration_ms": 5001},
                            {"keyboard_usage": 0x4F, "key": True}):
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    evidence.assert_key(f.keyboard, f.player, **{"key": Key.RIGHT, **changes})
            self.assertEqual(f.keyboard.commands, [])
            self.assertEqual(f.keyboard.contexts, 0)


if __name__ == "__main__":
    unittest.main()
