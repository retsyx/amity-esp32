"""FIFO evidence must distinguish real saturation from lost input or replay."""

from contextlib import contextmanager
from pathlib import Path
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Frame, Key
from amity_harness.ble_fifo import BLEFIFOEvidence, _HELD_USAGES, _KEYS, _USAGES
from amity_harness.diagnostics import LogCapture
import test_ble_input as input_fixture


class SaturatingKeyboard(input_fixture.Keyboard):
    def __init__(self, fixture, *, fault=None):
        super().__init__(fixture)
        self.fault, self.phase, self.groups = fault, "idle", 0

    @contextmanager
    def reports(self):
        self.groups += 1
        try:
            yield self
        finally:
            self.all_up()

    def report(self, *, keyboard=(), consumer=0):
        keyboard = tuple(keyboard)
        self.commands.append((keyboard, consumer))
        action = self.action("ble_report")
        if keyboard == _USAGES and consumer == 0:
            if self.fault == "non-key prefix":
                self.emit(self.f.avr, 0x71)
            for number, key in enumerate(_KEYS):
                recipient = self.f.tv if self.fault == "recipient" and not number else self.f.player
                self.emit(recipient, 0x44, 4 if self.fault == "prefix" and not number else key)
            self.emit(self.f.avr, 0x44, Key.MUTE)
            # Mute is admitted and may legitimately repeat while held.
            self.emit(self.f.avr, 0x44, Key.MUTE)
            if self.fault == "dropped press":
                self.emit(self.f.avr, 0x44, Key.VOLUME_DOWN)
            self.phase = "ten"
        elif keyboard == _HELD_USAGES and consumer == 0:
            # The old Down/Up frames were admitted before those physical ups.
            # Their completion remains legitimate; the other source holds run.
            self.emit(self.f.player, 0x44, Key.DOWN)
            self.emit(self.f.player, 0x44, Key.UP)
            self.emit(self.f.player, 0x44, Key.RIGHT)
            if self.fault == "capacity release":
                self.emit(self.f.player, 0x45)
            self.phase = "capacity"
        elif keyboard == (0x28,) and consumer == 0:
            self.emit(self.f.player, 0x44, Key.SELECT)
            if self.fault == "select old consumer":
                self.emit(self.f.avr, 0x44, Key.VOLUME_UP)
            elif self.fault == "select extra release":
                self.emit(self.f.avr, 0x45)
            self.phase = "fresh_select"
        elif not keyboard and consumer == 0xE2:
            self.emit(self.f.avr, 0x44, Key.MUTE)
            self.phase = "fresh_mute"
        elif not keyboard and not consumer:
            if self.phase == "barrier":
                # Already admitted frames legitimately finish after all-up,
                # before the final operand-free release in that same FIFO.
                self.emit(self.f.player, 0x44, Key.RIGHT)
                self.emit(self.f.player, 0x45)
                self.emit(self.f.avr, 0x45)
                if self.fault == "up replay":
                    self.emit(self.f.player, 0x44, Key.RIGHT)
                if self.fault == "failed up":
                    self.emit(self.f.avr, 0x45)
            elif self.phase == "fresh_select":
                self.emit(self.f.player, 0x45)
            elif self.phase == "fresh_mute":
                self.emit(self.f.avr, 0x45)
            self.phase = "up"
        return action

    def api_barrier(self, key, *, duration_ms=0):
        if key != Key.PLAY or duration_ms:
            raise AssertionError("The FIFO barrier must use a fresh shipping-API Play tap")
        if self.phase != "capacity" or self.commands[-1] != (_HELD_USAGES, 0):
            raise AssertionError("The rejected volume keys and six admitted holds must remain down through the barrier")
        action = self.action("key")
        if self.fault != "missing barrier":
            self.emit(self.f.player, 0x44, Key.PLAY)
        if self.fault == "capacity held replay":
            self.emit(self.f.player, 0x44, Key.DOWN)
        self.emit(self.f.avr, 0x44, Key.MUTE)
        if self.fault == "held replay":
            self.emit(self.f.avr, 0x44, Key.VOLUME_UP)
        self.phase = "barrier"
        return action


class FIFOTests(unittest.TestCase):
    @contextmanager
    def fixture(self, *, fault=None, log_fault=None, status_drop=False):
        base = input_fixture.BLEInputEvidenceTests()
        with base.fixture() as fixture:
            fixture.keyboard = SaturatingKeyboard(fixture, fault=fault)
            fixture.main.press_key = fixture.keyboard.api_barrier
            gpio = fixture.avr.dut.wiring.dut_cec_gpio(fixture.avr.dut.name,
                                                     fixture.avr.tentacle, fixture.avr.direction)
            initiator = fixture.scan.initiators[fixture.avr.gpio]
            frames = [Frame(initiator, fixture.avr.address.logical, 0x44, bytes([key])).raw
                      for key in (Key.VOLUME_UP, Key.VOLUME_DOWN)]
            if log_fault == "one drop":
                frames.pop()
            elif log_fault == "extra key drop":
                frames.append(Frame(initiator, fixture.player.address.logical, 0x44, b"\x04").raw)
            elif log_fault == "wrong GPIO":
                gpio = fixture.avr.gpio
            elif log_fault == "old drops":
                frames = []
            if status_drop:
                frames.append(Frame(initiator, fixture.avr.address.logical, 0x71).raw)
            if log_fault == "unexpected non-key":
                frames.append(Frame(initiator, fixture.avr.address.logical, 0x83).raw)
            logs = "baseline\n" + "".join("W cec: TX dropped: FIFO full (GPIO %d, %d bytes: %s)\n"
                                          % (gpio, len(raw), raw.hex(" ")) for raw in frames)
            captures = []
            original_state = fixture.main.get_state
            final_read = False

            def state():
                nonlocal final_read
                if len(captures) == 2 and not final_read:
                    final_read = True
                    if fault == "final owner release":
                        fixture.keyboard.emit(fixture.avr, 0x45)
                    elif fault == "final owner old consumer":
                        fixture.keyboard.emit(fixture.avr, 0x44, Key.VOLUME_UP)
                        fixture.keyboard.emit(fixture.avr, 0x45)
                return original_state()

            fixture.main.get_state = state

            def capture(*, expected_boot):
                self.assertEqual(expected_boot, fixture.scan._boot)
                stamp = time.monotonic_ns()
                header = ("\n===== Main Amity: bench =====\nDevice: " + fixture.main.identity.device_id
                          + "\nBoot: " + fixture.scan._boot + "\n\n")
                result = LogCapture((header + ("baseline\n" if not captures else logs)).encode(),
                                    fixture.main.identity.device_id, expected_boot, expected_boot, expected_boot,
                                    stamp, stamp + 1)
                captures.append(result)
                if len(captures) == 2:
                    if fault == "log release":
                        fixture.keyboard.emit(fixture.avr, 0x45)
                    elif fault == "log mute":
                        fixture.keyboard.emit(fixture.avr, 0x44, Key.MUTE)
                        fixture.keyboard.emit(fixture.avr, 0x45)
                return result

            fixture.main.capture_logs = capture
            yield fixture

    def test_exact_prefix_drops_no_replay_and_fresh_inputs_have_physical_and_log_proof(self):
        for status_drop in (False, True):
            with self.subTest(status_drop=status_drop), self.fixture(status_drop=status_drop) as fixture:
                with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                    evidence.assert_saturated(fixture.keyboard, source=fixture.player, volume=fixture.avr)
                check, = fixture.bed.control_observations[-1]["checks"]
                self.assertTrue(check["passed"] and check["drop_log_same_boot"])
                self.assertEqual(check["fifo_capacity"], 8)
                self.assertTrue(check["prefix_includes_all_dut_frames"])
                self.assertEqual(len(check["admitted_prefix"]), 8)
                self.assertEqual(len(check["dropped_key_frames"]), 2)
                self.assertEqual(len(check["non_key_drops"]), int(status_drop))
                self.assertEqual(check["saturation_report_count"], 1)
                self.assertEqual(check["source_barrier_key"], Key.PLAY)
                self.assertEqual(check["held_keyboard_usages"], list(_HELD_USAGES))
                self.assertEqual(fixture.keyboard.commands[1], (_HELD_USAGES, 0))
                self.assertTrue(all(usage in _HELD_USAGES for usage in (0x7F, 0x80, 0x81)))
                self.assertGreaterEqual(sum(row["frame"].endswith(":44:43")
                    and row["wire_timestamp_ns"] < check["initial_volume_release_wire_timestamp_ns"]
                    for row in check["frames"]), 3)
                self.assertEqual(fixture.keyboard.groups, 1)
                self.assertEqual(fixture.keyboard.commands[0], (_USAGES, 0))
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                self.assertIsNone(fixture.player.pressed_key)
                self.assertIsNone(fixture.avr.pressed_key)

    def test_wrong_prefix_or_recipient_and_dropped_held_or_released_replay_fail(self):
        for fault in ("prefix", "non-key prefix", "recipient", "dropped press", "held replay", "up replay", "failed up", "missing barrier", "capacity release", "capacity held replay"):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                with self.assertRaises((AssertionError, TimeoutError)):
                    with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                        evidence.assert_saturated(fixture.keyboard, source=fixture.player, volume=fixture.avr)
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))

    def test_saturation_needs_exact_fresh_drop_logs_on_the_actual_dut_gpio(self):
        for fault in ("one drop", "extra key drop", "wrong GPIO", "old drops", "unexpected non-key"):
            with self.subTest(fault=fault), self.fixture(log_fault=fault) as fixture:
                with self.assertRaises(AssertionError):
                    with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                        evidence.assert_saturated(fixture.keyboard, source=fixture.player, volume=fixture.avr)
                self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])

    def test_late_consumer_presses_and_extra_releases_cannot_hide_in_fresh_input_or_slow_reads(self):
        for fault in ("select old consumer", "select extra release", "log release", "log mute",
                      "final owner release", "final owner old consumer"):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                with self.assertRaises(AssertionError):
                    with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                        evidence.assert_saturated(fixture.keyboard, source=fixture.player, volume=fixture.avr)
                self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])


if __name__ == "__main__":
    unittest.main()
