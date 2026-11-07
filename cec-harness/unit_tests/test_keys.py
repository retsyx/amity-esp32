"""Bounded UI key actions require original directed physical press/release."""

from dataclasses import replace
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, Frame, Hooks, Key, PowerState
from test_activity import Fixture


class KeyTests(unittest.TestCase):
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
            return {"ok": True}

        f.dut.request = request
        return f

    def receive(self, f, component, key, *, transport="wire", timestamp=None, source=None,
                release=True):
        initiator = f.dut.last_scan.initiators[component.gpio] if source is None else source
        stamp = time.monotonic_ns() if timestamp is None else timestamp
        component.receive(Frame(initiator, component.address.logical, 0x44, bytes([key]),
                                timestamp_ns=stamp), transport=transport)
        if release:
            component.receive(Frame(initiator, component.address.logical, 0x45,
                                    timestamp_ns=stamp + 1), transport=transport)

    def test_zero_duration_tap_and_bounded_hold_use_the_shipping_api(self):
        for duration in (0, 100, 5000):
            with self.subTest(duration=duration):
                f = self.fixture()
                action = f.dut.press_key(Key.VOLUME_UP, duration_ms=duration)
                self.assertEqual(f.calls, [("/api/key", {"key": 0x41, "duration_ms": duration})])
                self.receive(f, f.avr, Key.VOLUME_UP)
                self.assertIs(action.assert_received_by(f.avr, timeout=.01), action)
                self.assertTrue(f.dut.key_operations[0]["released"])
                self.assertEqual(action.trace["press_frame"], "05:44:41")
                self.assertEqual(action.trace["release_frame"], "05:45")
                self.assertFalse(f.callbacks)  # Firmware owns the bounded release.

    def test_tv_front_wire_volume_and_mute_proof_works_for_main_and_member(self):
        for remote in (False, True):
            with self.subTest(remote=remote):
                f = self.fixture(remote=remote)
                tv = f.member_tv if remote else f.tv
                initial_volume = tv.volume
                for key in (Key.VOLUME_UP, Key.MUTE):
                    action = f.dut.press_key(key)
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
        action = f.dut.press_key(Key.VOLUME_UP)
        self.receive(f, f.tv, Key.VOLUME_UP, transport="virtual")
        self.assertEqual(f.tv.volume, 26)
        with self.assertRaises(TimeoutError):
            action.assert_received_by(f.tv, timeout=.005)

    def test_held_repetition_needs_a_release_after_the_final_press(self):
        f = self.fixture()
        action = f.dut.press_key(Key.RIGHT)
        self.receive(f, f.player, Key.RIGHT)
        self.receive(f, f.player, Key.RIGHT, release=False)
        with self.assertRaises(TimeoutError):
            action.assert_received_by(f.player, timeout=.005)
        f.player.receive(Frame(0, f.player.address.logical, 0x45, timestamp_ns=time.monotonic_ns()))
        action.assert_received_by(f.player, timeout=.01)
        self.assertEqual(action.trace["presses"], 2)

    def test_virtual_stale_wrong_source_and_missing_release_cannot_prove_a_key(self):
        for wrong in ("virtual", "stale", "source", "release", "dropped", "rewritten"):
            with self.subTest(wrong=wrong):
                f = self.fixture()
                action = f.dut.press_key(Key.SELECT)
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
                action = f.dut.press_key(Key.VOLUME_UP)
                self.receive(f, target, Key.VOLUME_UP)
                self.receive(f, f.player, Key.VOLUME_UP)
                with self.assertRaisesRegex(AssertionError, "wrong physical recipient"):
                    action.assert_received_by(target, timeout=.01)

    def test_member_key_evidence_uses_main_ui_exact_owner_boot_and_scan(self):
        f = self.fixture(remote=True)
        action = f.dut.press_key(Key.PLAY)
        self.receive(f, f.member_player, Key.PLAY)
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
                action = f.dut.press_key(Key.SELECT)
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
        action = f.dut.press_key(Key.POWER)
        with self.assertRaises(TimeoutError):
            action.wait_for_standby()
        f.current = None
        self.assertIs(action.wait_for_standby(), action)
        self.assertTrue(action.trace["standby"])
        self.assertEqual(action.kind, "key")
        self.assertIs(action.session, f.session)

    def test_invalid_key_and_duration_are_rejected_before_network_io(self):
        for key in (-1, 256, True, "1", 1.0):
            with self.subTest(key=key):
                f = self.fixture()
                with self.assertRaises(ValueError):
                    f.dut.press_key(key)
                self.assertFalse(f.calls)
        for duration in (-1, 5001, True, "100", 100.0, None):
            with self.subTest(duration=duration):
                f = self.fixture()
                with self.assertRaises(ValueError):
                    f.dut.press_key(Key.SELECT, duration_ms=duration)
                self.assertFalse(f.calls)
        with self.assertRaises(RuntimeError):
            DUT("amity-dut").press_key(Key.SELECT)

    def test_slow_http_uses_response_for_cec_budget_and_original_request_for_freshness(self):
        for physical in ("during_request", "after_response", "stale", "late"):
            for duration_ms in (0, 750):
                with self.subTest(physical=physical, duration_ms=duration_ms):
                    f = self.fixture()
                    clock = [time.monotonic_ns()]
                    started = clock[0]
                    completed = started + duration_ms * 1_000_000 + 2_000_000_000

                    def request(path, data=None, **kwargs):
                        f.calls.append((path, data))
                        self.assertEqual(path, "/api/key")
                        self.assertEqual(kwargs, {})  # Keep the shipping HTTP helper's own deadline.
                        if physical == "during_request":
                            self.receive(f, f.player, Key.RIGHT, timestamp=started + 50_000_000)
                        clock[0] = completed
                        return {"ok": True}

                    f.dut.request = request
                    with patch("amity_harness.keys.time.monotonic_ns", side_effect=lambda: clock[0]):
                        action = f.dut.press_key(Key.RIGHT, duration_ms=duration_ms)
                        if physical != "during_request":
                            stamp = {"after_response": completed + 100_000_000,
                                     "stale": started - 1,
                                     "late": completed + 1_000_000_001}[physical]
                            self.receive(f, f.player, Key.RIGHT, timestamp=stamp)
                        clock[0] = completed + 2_000_000_000
                        if physical in ("stale", "late"):
                            with self.assertRaises(TimeoutError):
                                action.assert_received_by(f.player, timeout=1)
                        else:
                            action.assert_received_by(f.player, timeout=1)
                            self.assertEqual(action.trace["deadline_monotonic_ns"], completed + 1_000_000_000)
                        self.assertEqual(action.started_ns, started)
                        self.assertEqual(action.response_completed_ns, completed)
                        self.assertEqual(action.trace["response_completed_monotonic_ns"], completed)
                        self.assertEqual(len(f.calls), 1)

    def test_http_failure_or_invalid_admission_never_replays_a_key(self):
        for response in ({"ok": False}, {}, TimeoutError("HTTP request deadline expired")):
            with self.subTest(response=response):
                f = self.fixture()

                def request(path, data=None, **kwargs):
                    f.calls.append((path, data))
                    if isinstance(response, Exception):
                        raise response
                    return response

                f.dut.request = request
                with self.assertRaises(TimeoutError if isinstance(response, Exception) else AssertionError):
                    f.dut.press_key(Key.RIGHT)
                self.assertEqual(len(f.calls), 1)

    def test_owner_validation_cannot_extend_the_original_physical_key_window(self):
        for late_wire in (False, True):
            with self.subTest(late_wire=late_wire):
                f = self.fixture()
                action = f.dut.press_key(Key.SELECT)
                action.started_ns -= 1_000_000_000  # Identity proof finishes after the window.
                action.response_completed_ns -= 1_000_000_000
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
