"""Remote key actions require original directed physical press/release."""

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, Frame, Hooks, Key, PowerState
from amity_harness.ble_keyboard import PairedKeyboard
from amity_harness.keys import HOLD_REPEAT_MIN_MS, _prepare_key
from test_activity import Fixture
from unit_tests.virtual_time import use_virtual_time


class Remote:
    """The paired remote's HID report boundary; pairing is tested separately."""

    press = PairedKeyboard.press

    def __init__(self, dut):
        self.dut, self._disconnected = dut, False
        self.sent, self.on_report, self.confirmed = [], None, 0

    def reports(self):
        remote = self

        class Reports:
            def __enter__(self):
                return self

            def report(self, *, keyboard=(), consumer=0):
                started = time.monotonic_ns()
                remote.sent.append((tuple(keyboard), consumer, started))
                if remote.on_report is not None:
                    remote.on_report(tuple(keyboard))
                return SimpleNamespace(started_ns=started)

            def all_up(self):
                return self.report()

            def __exit__(self, *args):
                remote.confirmed += 1
                return False

        return Reports()


class KeyTests(unittest.TestCase):
    def setUp(self):
        self.clock = use_virtual_time(self)

    def fixture(self, *, remote=False):
        f = Fixture()
        if remote:
            f.add_member()
        f.session.components = f.source.components
        f.session._stop = threading.Event()
        f.session._lock = threading.RLock()
        f.session.records = list(f.session.records)
        f.session.power_changed = lambda *args: None

        def record(component, direction, frame, *, transport="wire", **extra):
            f.session.records.append({"timestamp": time.monotonic(), "direction": direction,
                                      "component": component.name, "gpio": component.gpio,
                                      "frame": frame.raw.hex(":"), "source": frame.source,
                                      "destination": frame.destination, "opcode": frame.opcode,
                                      "transport": transport,
                                      "wire_timestamp_ns": frame.timestamp_ns if transport == "wire" else 0,
                                      **extra})

        f.session._record = record
        for component in f.source.components:
            component._power = PowerState.ON
        f.calls = []

        def request(path, data=None, **kwargs):
            f.calls.append((path, data))
            raise AssertionError("Keys use no HTTP request: " + path)

        f.dut.request = request
        f.remote = Remote(f.dut)
        return f

    def receive(self, f, component, key, *, transport="wire", timestamp=None, source=None,
                release=True, presses=1):
        initiator = f.dut.last_scan.initiators[component.gpio] if source is None else source
        stamp = time.monotonic_ns() if timestamp is None else timestamp
        for index in range(presses):
            component.receive(Frame(initiator, component.address.logical, 0x44, bytes([key]),
                                    timestamp_ns=stamp + index), transport=transport)
        if release:
            component.receive(Frame(initiator, component.address.logical, 0x45,
                                    timestamp_ns=stamp + presses), transport=transport)

    def test_remote_tap_and_bounded_hold_send_hid_down_then_up(self):
        for duration in (0, 100, 5000):
            with self.subTest(duration=duration):
                f = self.fixture()
                action = f.remote.press(Key.VOLUME_UP, duration_ms=duration)
                (down, down_consumer, pressed), (up, up_consumer, released) = f.remote.sent
                self.assertEqual((down, down_consumer, up, up_consumer), ((0x80,), 0, (), 0))
                self.assertGreaterEqual(released - pressed, duration * 1_000_000)
                self.assertEqual((action.started_ns, action.input_completed_ns), (pressed, released))
                self.assertEqual(f.remote.confirmed, 1)
                self.receive(f, f.avr, Key.VOLUME_UP, presses=2 if duration >= HOLD_REPEAT_MIN_MS else 1)
                self.assertIs(action.assert_received_by(f.avr, timeout=.01), action)
                self.assertTrue(f.dut.key_operations[0]["released"])
                self.assertEqual(f.dut.key_operations[0]["interface"], "ble")
                self.assertEqual(action.trace["press_frame"], "05:44:41")
                self.assertEqual(action.trace["release_frame"], "05:45")
                self.assertEqual(action.kind, "ble_key")
                self.assertEqual(f.calls, [])

    def test_remote_buttons_use_the_shipping_keyboard_translation(self):
        expected = {Key.SELECT: 0x28, Key.UP: 0x52, Key.DOWN: 0x51, Key.LEFT: 0x50, Key.RIGHT: 0x4F,
                    Key.POWER: 0x66, Key.VOLUME_UP: 0x80, Key.VOLUME_DOWN: 0x81, Key.MUTE: 0x7F}
        for key, usage in expected.items():
            with self.subTest(key=key):
                f = self.fixture()
                f.remote.press(key)
                self.assertEqual([row[:2] for row in f.remote.sent], [((usage,), 0), ((), 0)])

    def test_tv_front_wire_volume_and_mute_proof_works_for_main_and_member(self):
        for remote in (False, True):
            with self.subTest(remote=remote):
                f = self.fixture(remote=remote)
                tv = f.member_tv if remote else f.tv
                initial_volume = tv.volume
                for key in (Key.VOLUME_UP, Key.MUTE):
                    action = f.remote.press(key)
                    self.receive(f, tv, key)
                    self.assertIs(action.assert_received_by(tv, timeout=.01), action)
                    self.assertEqual(action.trace["press_frame"], f"40:44:{key:02x}")
                    self.assertEqual(action.trace["release_frame"], "40:45")
                    self.assertEqual(action.trace["owner_id"], tv.dut.identity.device_id)
                self.assertEqual(tv.volume, initial_volume + 1)
                self.assertTrue(tv.muted)
                self.assertEqual(f.avr.volume, 25)

    def test_tv_volume_model_changed_by_virtual_key_is_not_physical_key_proof(self):
        f = self.fixture()
        action = f.remote.press(Key.VOLUME_UP)
        self.receive(f, f.tv, Key.VOLUME_UP, transport="virtual")
        self.assertEqual(f.tv.volume, 26)
        with self.assertRaises(TimeoutError):
            action.assert_received_by(f.tv, timeout=.005)

    def test_held_repetition_needs_a_release_after_the_final_press(self):
        f = self.fixture()
        action = f.remote.press(Key.RIGHT, duration_ms=HOLD_REPEAT_MIN_MS)
        self.receive(f, f.player, Key.RIGHT)
        self.receive(f, f.player, Key.RIGHT, release=False)
        with self.assertRaises(TimeoutError):
            action.assert_received_by(f.player, timeout=.005)
        f.player.receive(Frame(0, f.player.address.logical, 0x45, timestamp_ns=time.monotonic_ns()))
        action.assert_received_by(f.player, timeout=.01)
        self.assertEqual(action.trace["presses"], 2)

    def test_hold_requires_repetition_and_only_a_complete_tap_command_requires_one_press(self):
        # A remote tap's down and up are separate reports; repetition between
        # them depends on delivery timing. Matter and HomeKit send one command.
        for complete, duration, presses, accepted in (
                (False, 0, 1, True), (False, 0, 2, True), (False, HOLD_REPEAT_MIN_MS - 1, 1, True),
                (False, HOLD_REPEAT_MIN_MS, 1, False), (False, HOLD_REPEAT_MIN_MS, 2, True),
                (True, 0, 1, True), (True, 0, 2, False)):
            with self.subTest(complete=complete, duration=duration, presses=presses):
                f = self.fixture()
                action = (_prepare_key(f.dut, Key.RIGHT, duration) if complete
                          else f.remote.press(Key.RIGHT, duration_ms=duration))
                self.assertIs(action.complete_tap, complete)
                self.receive(f, f.player, Key.RIGHT, presses=presses)
                if accepted:
                    self.assertIs(action.assert_received_by(f.player, timeout=.01), action)
                    self.assertEqual(action.trace["presses"], presses)
                else:
                    with self.assertRaises(AssertionError):
                        action.assert_received_by(f.player, timeout=.01)
                    self.assertEqual(action.trace["presses"], presses)
                    self.assertNotIn("released", action.trace)

    def test_virtual_stale_wrong_source_and_missing_release_cannot_prove_a_key(self):
        for wrong in ("virtual", "stale", "source", "release", "dropped", "rewritten"):
            with self.subTest(wrong=wrong):
                f = self.fixture()
                action = f.remote.press(Key.SELECT)
                if wrong == "dropped":
                    f.player.hooks = Hooks(before_rx=lambda component, frame: None)
                elif wrong == "rewritten":
                    f.player.hooks = Hooks(before_rx=lambda component, frame:
                                          replace(frame, operands=bytes([Key.DOWN])) if frame.opcode == 0x44 else frame)
                self.receive(f, f.player, Key.SELECT,
                             transport="virtual" if wrong == "virtual" else "wire",
                             timestamp=1 if wrong == "stale" else None,
                             source=8 if wrong == "source" else None,
                             release=wrong != "release")
                with self.assertRaises(TimeoutError):
                    action.assert_received_by(f.player, timeout=.005)

    def test_expected_recipient_does_not_hide_the_same_key_sent_elsewhere(self):
        for remote in (False, True):
            with self.subTest(remote=remote):
                f = self.fixture(remote=remote)
                target = f.member_avr if remote else f.avr
                action = f.remote.press(Key.VOLUME_UP)
                self.receive(f, target, Key.VOLUME_UP)
                self.receive(f, f.player, Key.VOLUME_UP)
                with self.assertRaisesRegex(AssertionError, "wrong physical recipient"):
                    action.assert_received_by(target, timeout=.01)

    def test_member_key_evidence_uses_main_ui_exact_owner_boot_and_scan(self):
        f = self.fixture(remote=True)
        action = f.remote.press(Key.SELECT)
        self.receive(f, f.member_player, Key.SELECT)
        action.assert_received_by(f.member_player, timeout=.01)
        self.assertEqual(action.trace["owner_id"], f.member.identity.device_id)
        self.assertEqual(action.owner_boots[f.member.identity.device_id], f.member_boot)
        self.assertIs(action.scan, f.dut.last_scan)
        f.source.system["membership"]["peers"][0]["boot_id"] = "e" * 32
        with self.assertRaises(AssertionError):
            action.assert_received_by(f.member_player, timeout=.01)

    def test_boot_session_and_configuration_changes_invalidate_key_proof(self):
        for mutation in ("boot", "session", "configuration", "activity"):
            with self.subTest(mutation=mutation):
                f = self.fixture()
                action = f.remote.press(Key.SELECT)
                self.receive(f, f.player, Key.SELECT)
                if mutation == "boot":
                    f.source.system["boot_id"] = "e" * 32
                elif mutation == "session":
                    f.bed.session = object()
                elif mutation == "configuration":
                    f.bump()
                else:
                    f.current = f.rows[0]["activity_id"]
                with self.assertRaises(AssertionError):
                    action.assert_received_by(f.player, timeout=.01)

    def test_power_requires_ui_standby_and_preserves_its_wire_boundary(self):
        f = self.fixture(remote=True)
        f.current = f.rows[0]["activity_id"]
        action = f.remote.press(Key.POWER)
        self.assertEqual(f.remote.sent[0][:2], ((0x66,), 0))
        with self.assertRaises(TimeoutError):
            action.wait_for_standby()
        f.current = None
        self.assertIs(action.wait_for_standby(), action)
        self.assertTrue(action.trace["standby"])
        self.assertEqual(action.kind, "ble_key")
        self.assertIs(action.session, f.session)

    def test_keys_the_remote_cannot_send_and_invalid_durations_send_no_report(self):
        for key in (-1, 256, True, "1", 1.0, Key.PLAY, Key.PAUSE, Key.STOP, Key.PLAY_PAUSE):
            with self.subTest(key=key):
                f = self.fixture()
                with self.assertRaises(ValueError):
                    f.remote.press(key)
                self.assertEqual((f.remote.sent, f.dut.key_operations), ([], []))
        for duration in (-1, 5001, True, "100", 100.0, None):
            with self.subTest(duration=duration):
                f = self.fixture()
                with self.assertRaises(ValueError):
                    f.remote.press(Key.SELECT, duration_ms=duration)
                self.assertEqual(f.remote.sent, [])
        f = self.fixture()
        f.remote._disconnected = True
        with self.assertRaisesRegex(RuntimeError, "disconnected"):
            f.remote.press(Key.SELECT)
        with self.assertRaises(RuntimeError):
            Remote(DUT("amity-dut")).press(Key.SELECT)
        self.assertEqual(f.remote.sent, [])

    def test_freshness_starts_at_the_hid_down_and_cec_budget_at_its_up(self):
        for physical in ("during_hold", "after_up", "stale", "late"):
            for duration_ms in (0, 750):
                with self.subTest(physical=physical, duration_ms=duration_ms):
                    f = self.fixture()

                    def report(keyboard):
                        if keyboard and physical == "during_hold":
                            self.receive(f, f.player, Key.RIGHT, timestamp=time.monotonic_ns() + 50_000_000,
                                         presses=2 if duration_ms else 1)
                        # Each notification's worker round trip takes two seconds.
                        self.clock.advance(2)

                    f.remote.on_report = report
                    before = self.clock.now_ns
                    action = f.remote.press(Key.RIGHT, duration_ms=duration_ms)
                    started, completed = action.started_ns, action.input_completed_ns
                    self.assertEqual((started, completed), (f.remote.sent[0][2], f.remote.sent[1][2]))
                    self.assertTrue(before <= started < completed)
                    self.assertGreaterEqual(completed - started, max(2_000_000_000, duration_ms * 1_000_000))
                    if physical != "during_hold":
                        stamp = {"after_up": completed + 100_000_000,
                                 "stale": started - 2,
                                 "late": completed + 1_000_000_001}[physical]
                        self.receive(f, f.player, Key.RIGHT, timestamp=stamp,
                                     presses=2 if duration_ms else 1)
                    self.clock.advance(2)
                    if physical in ("stale", "late"):
                        with self.assertRaises(TimeoutError):
                            action.assert_received_by(f.player, timeout=1)
                    else:
                        action.assert_received_by(f.player, timeout=1)
                        self.assertEqual(action.trace["deadline_monotonic_ns"], completed + 1_000_000_000)
                    self.assertEqual(action.trace["started_monotonic_ns"], started)
                    self.assertEqual(action.trace["input_completed_monotonic_ns"], completed)
                    self.assertEqual(len(f.remote.sent), 2)

    def test_failed_report_never_replays_a_key(self):
        f = self.fixture()

        def report(keyboard):
            raise TimeoutError("BLE keyboard worker deadline expired")

        f.remote.on_report = report
        with self.assertRaises(TimeoutError):
            f.remote.press(Key.RIGHT)
        self.assertEqual([row[:2] for row in f.remote.sent], [((0x4F,), 0)])
        self.assertEqual(f.remote.confirmed, 1)  # The report context still releases HID state.

    def test_owner_validation_cannot_extend_the_original_physical_key_window(self):
        for late_wire in (False, True):
            with self.subTest(late_wire=late_wire):
                f = self.fixture()
                action = f.remote.press(Key.SELECT)
                action.started_ns -= 1_000_000_000  # Identity proof finishes after the window.
                action.input_completed_ns -= 1_000_000_000
                timestamp = action.started_ns + (10_000_000 if late_wire else 100)
                self.receive(f, f.player, Key.SELECT, timestamp=timestamp)
                if late_wire:
                    with self.assertRaises(TimeoutError):
                        action.assert_received_by(f.player, timeout=.005)
                else:
                    action.assert_received_by(f.player, timeout=.005)
                    self.assertLess(action.trace["release_wire_timestamp_ns"], action.trace["deadline_monotonic_ns"])


if __name__ == "__main__":
    unittest.main()
