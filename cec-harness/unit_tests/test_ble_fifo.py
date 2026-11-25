"""FIFO evidence must prove ten-key admission and distinguish it from lost input or replay."""

from contextlib import contextmanager
from pathlib import Path
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Frame, Key
from amity_harness.ble_fifo import BLEFIFOEvidence, _CONSUMER_KEYS, _HELD_USAGES, _KEYS, _USAGES
from amity_harness.diagnostics import LogCapture
import test_ble_input as input_fixture
from unit_tests.virtual_time import use_virtual_time


# Source keys that stay down with Mute and both volume keys until all-up.
_HELD_SOURCE_KEYS = tuple(key for key in _KEYS if key not in (Key.DOWN, Key.UP))


class TenKeyKeyboard(input_fixture.Keyboard):
    """The 16-slot FIFO admits every press of one report; holds repeat in turn."""

    def __init__(self, fixture, *, clock, fault=None):
        super().__init__(fixture)
        self.fault, self.phase, self.groups = fault, "idle", 0
        # Timed tests give each frame this many seconds of wire time.
        self.clock, self.period = clock, 0

    def emit(self, target, opcode, key=None):
        if self.period:
            self.clock.advance(self.period)
        super().emit(target, opcode, key)

    @contextmanager
    def reports(self):
        self.groups += 1
        try:
            yield self
        finally:
            self.all_up()

    def hold_round(self, *, source=_HELD_SOURCE_KEYS, consumer=_CONSUMER_KEYS):
        # Each held key's next press enters the FIFO when its previous press
        # completes, so the continuing holds transmit one press each in turn.
        for key in source:
            self.emit(self.f.player, 0x44, key)
        for key in consumer:
            self.emit(self.f.avr, 0x44, key)

    def report(self, *, keyboard=(), consumer=0):
        keyboard = tuple(keyboard)
        self.commands.append((keyboard, consumer))
        action = self.action("ble_report")
        # A stream that stops at the barrier's up still finishes the press it
        # had queued behind the barrier, then queues nothing more.
        held_consumer = _CONSUMER_KEYS[:-1] if self.fault == "volume down stopped at barrier up" else _CONSUMER_KEYS
        if keyboard == _USAGES and consumer == 0:
            presses = [(self.f.player, key) for key in _KEYS] + [(self.f.avr, key) for key in _CONSUMER_KEYS]
            if self.fault == "non-key prefix":
                self.emit(self.f.avr, 0x71)
            elif self.fault == "prefix":
                presses[0] = (self.f.player, Key.RIGHT)
            elif self.fault == "recipient":
                presses[0] = (self.f.tv, Key.SELECT)
            elif self.fault == "misordered prefix":
                presses[-2], presses[-1] = presses[-1], presses[-2]
            elif self.fault == "dropped press":
                # A refused press never reaches the wire and ends its hold.
                presses.pop()
            for recipient, key in presses:
                self.emit(recipient, 0x44, key)
            # Admitted consumer holds may legitimately repeat while held.
            self.hold_round(source=(), consumer=[key for recipient, key in presses if recipient is self.f.avr])
            self.phase = "ten"
        elif keyboard == _HELD_USAGES and consumer == 0 and self.phase == "ten":
            # The old Down/Up frames were admitted before those physical ups.
            # Their completion remains legitimate; the other eight holds run.
            self.emit(self.f.player, 0x44, Key.DOWN)
            self.emit(self.f.player, 0x44, Key.UP)
            self.hold_round()
            if self.fault == "capacity release":
                self.emit(self.f.player, 0x45)
            self.phase = "capacity"
        elif keyboard == (*_HELD_USAGES, 0x51) and consumer == 0:
            if self.phase != "capacity" or self.commands[-2] != (_HELD_USAGES, 0):
                raise AssertionError("Both volume keys and six other holds must remain down through the barrier")
            # The fresh Down press waits behind one press of each continuing hold.
            self.hold_round()
            if self.fault != "missing barrier":
                self.emit(self.f.player, 0x44, Key.DOWN)
            if self.fault == "capacity held replay":
                self.emit(self.f.player, 0x44, Key.UP)
            self.phase = "barrier_down"
        elif keyboard == _HELD_USAGES and consumer == 0 and self.phase == "barrier_down":
            # The continuing holds keep running behind the barrier. A Down
            # repeat admitted before this up may still finish.
            self.hold_round()
            self.emit(self.f.player, 0x44, Key.DOWN)
            self.phase = "barrier"
        elif keyboard == (0x28,) and consumer == 0:
            self.emit(self.f.player, 0x44, Key.SELECT)
            if self.fault == "select old consumer":
                self.emit(self.f.avr, 0x44, Key.VOLUME_UP)
            elif self.fault == "select extra release":
                self.emit(self.f.avr, 0x45)
            self.phase = "fresh_select"
        elif not keyboard and consumer == 0xE2:
            if self.fault == "mute old consumer":
                self.emit(self.f.avr, 0x44, Key.VOLUME_DOWN)
            self.emit(self.f.avr, 0x44, Key.MUTE)
            self.phase = "fresh_mute"
        elif not keyboard and not consumer:
            if self.phase == "barrier":
                if self.fault == "late barrier repeat":
                    self.emit(self.f.player, 0x44, Key.DOWN)
                # Each running hold's queued press legitimately finishes after
                # all-up, before each operand-free final release in that FIFO.
                self.hold_round(consumer=held_consumer)
                if self.fault != "missing source release":
                    self.emit(self.f.player, 0x45)
                if self.fault != "missing volume release":
                    self.emit(self.f.avr, 0x45)
                if self.fault == "up replay":
                    self.emit(self.f.player, 0x44, Key.RIGHT)
                elif self.fault == "source after release":
                    self.emit(self.f.player, 0x44, Key.RIGHT)
                    self.emit(self.f.player, 0x45)
                elif self.fault == "failed up":
                    self.emit(self.f.avr, 0x45)
                elif self.fault == "volume after release":
                    self.emit(self.f.avr, 0x44, Key.VOLUME_DOWN)
                    self.emit(self.f.avr, 0x45)
            elif self.phase == "fresh_select":
                self.emit(self.f.player, 0x45)
            elif self.phase == "fresh_mute":
                self.emit(self.f.avr, 0x45)
            self.phase = "up"
        return action


class FIFOTests(unittest.TestCase):
    def setUp(self):
        self.clock = use_virtual_time(self)

    @contextmanager
    def fixture(self, *, fault=None, log_fault=None):
        base = input_fixture.BLEInputEvidenceTests()
        with base.fixture() as fixture:
            fixture.keyboard = TenKeyKeyboard(fixture, clock=self.clock, fault=fault)
            gpio = fixture.avr.dut.wiring.dut_cec_gpio(fixture.avr.dut.name, fixture.avr.tentacle,
                                                       fixture.avr.direction)
            front = fixture.tv.dut.wiring.dut_cec_gpio(fixture.tv.dut.name, fixture.tv.tentacle, fixture.tv.direction)
            initiator = fixture.scan.initiators[fixture.avr.gpio]
            avr = fixture.avr.address.logical
            drops = {"volume drop": [(gpio, Frame(initiator, avr, 0x44, bytes([Key.VOLUME_DOWN])).raw)],
                     "status drop": [(gpio, Frame(initiator, avr, 0x71).raw)],
                     "release drop": [(gpio, Frame(initiator, avr, 0x45).raw)],
                     "other wire drop": [(front, Frame(fixture.scan.initiators[fixture.tv.gpio],
                                                       fixture.tv.address.logical, 0x44, b"\x00").raw)],
                     "old drops": [(gpio, Frame(initiator, avr, 0x44, bytes([key])).raw)
                                   for key in (Key.VOLUME_UP, Key.VOLUME_DOWN)]}.get(log_fault, [])
            fixture.expected_drops = [{"gpio": wire, "frame": raw.hex(":")} for wire, raw in drops]
            lines = "".join("W cec: TX dropped: FIFO full (GPIO %d, %d bytes: %s)\n"
                            % (wire, len(raw), raw.hex(" ")) for wire, raw in drops)
            # Drops logged before the check's first capture are not fresh.
            baseline = "baseline\n" + (lines if log_fault == "old drops" else "")
            fresh = "fresh\n" + ("" if log_fault == "old drops" else lines)
            if log_fault == "unrecognized drop":
                fresh += "W cec: TX dropped (queue full) GPIO %d: 05 44 42\n" % gpio
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
                result = LogCapture((header + baseline + (fresh if captures else "")).encode(),
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

    def assert_fails(self, fixture, error, message):
        with self.assertRaisesRegex(error, message):
            with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                evidence.assert_ten_key_admission(fixture.keyboard, source=fixture.player, volume=fixture.avr)
        self.assertFalse(fixture.bed.control_observations[-1]["checks"][-1]["passed"])
        self.assertEqual(fixture.keyboard.commands[-1], ((), 0))

    def test_exact_ten_frame_prefix_no_drop_no_replay_and_fresh_inputs_have_physical_and_log_proof(self):
        for log_fault in (None, "old drops"):
            with self.subTest(log_fault=log_fault), self.fixture(log_fault=log_fault) as fixture:
                with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                    evidence.assert_ten_key_admission(fixture.keyboard, source=fixture.player, volume=fixture.avr)
                check, = fixture.bed.control_observations[-1]["checks"]
                initiator = fixture.scan.initiators[fixture.avr.gpio]
                prefix = [Frame(initiator, fixture.player.address.logical, 0x44, bytes([key])).raw.hex(":")
                          for key in _KEYS]
                consumer = [Frame(initiator, fixture.avr.address.logical, 0x44, bytes([key])).raw.hex(":")
                            for key in _CONSUMER_KEYS]
                self.assertTrue(check["passed"] and check["drop_log_same_boot"])
                self.assertEqual(check["kind"], "fifo_admission")
                self.assertTrue(check["prefix_includes_all_dut_frames"])
                self.assertEqual(check["admitted_prefix"], prefix + consumer)
                self.assertEqual(check["fifo_drops"], [])
                self.assertEqual(check["ten_key_report_count"], 1)
                self.assertEqual(check["ten_key_usages"], list(_USAGES))
                self.assertEqual(check["source_barrier_key"], Key.DOWN)
                self.assertEqual(check["held_keyboard_usages"], list(_HELD_USAGES))
                self.assertEqual(fixture.keyboard.commands[1:4], [(_HELD_USAGES, 0), ((*_HELD_USAGES, 0x51), 0),
                                                                  (_HELD_USAGES, 0)])
                self.assertTrue(all(usage in _HELD_USAGES for usage in (0x7F, 0x80, 0x81)))
                # Admitted consumer holds legitimately repeat until their release.
                for frame in consumer:
                    self.assertGreaterEqual(sum(row["frame"] == frame
                        and row["wire_timestamp_ns"] < check["initial_volume_release_wire_timestamp_ns"]
                        for row in check["frames"]), 3)
                self.assertEqual(fixture.keyboard.groups, 1)
                self.assertEqual(fixture.keyboard.commands[0], (_USAGES, 0))
                self.assertEqual(fixture.keyboard.commands[-1], ((), 0))
                self.assertIsNone(fixture.player.pressed_key)
                self.assertIsNone(fixture.avr.pressed_key)

    def test_both_volume_streams_add_two_measured_press_periods_to_each_wait_behind_the_holds(self):
        # Each report reaches the owner 3.5 ms after its boundary, and each
        # frame then takes 1 ms of the 12 ms budget on the wire. The tenth
        # press, the fresh Down, its repeat and both final releases land past
        # the budget but within two press periods of it. Delivering any of
        # those reports 2 ms later fails its bound.
        steps = {((_USAGES, 0), "idle"): "ten",
                 (((*_HELD_USAGES, 0x51), 0), "capacity"): "barrier_down",
                 ((_HELD_USAGES, 0), "barrier_down"): "barrier_up",
                 (((), 0), "barrier"): "held_all_up"}
        late = {"ten": (TimeoutError, "No exact ten-frame physical FIFO admission prefix"),
                "barrier_down": (TimeoutError, "No fresh physical BLE key press"),
                "barrier_up": (AssertionError, "A released navigation hold replayed after the fresh FIFO barrier"),
                "held_all_up": (TimeoutError, "No fresh physical BLE key release")}
        for slow in (None, *late):
            with self.subTest(slow=slow), self.fixture() as fixture:
                keyboard = fixture.keyboard
                keyboard.period = .001

                def deliver():
                    step = steps.get((keyboard.commands[-1], keyboard.phase))
                    self.clock.advance(.0055 if step == slow else .0035)

                keyboard.delivery = deliver
                if slow:
                    self.assert_fails(fixture, *late[slow])
                    continue
                with BLEFIFOEvidence(fixture.main, fixture.scan) as evidence:
                    evidence.assert_ten_key_admission(keyboard, source=fixture.player, volume=fixture.avr)
                check, = fixture.bed.control_observations[-1]["checks"]
                changes = {change["phase"]: change for change in check["input_changes"]}
                self.assertTrue(check["passed"])
                self.assertTrue(1_000_000 <= check["press_period_ns"] < 1_001_000)
                # Without the two press periods, these waits would have failed.
                self.assertGreater(check["source_barrier_wire_timestamp_ns"],
                                   changes["source_barrier_down"]["deadline_monotonic_ns"])
                self.assertGreater(check["initial_volume_release_wire_timestamp_ns"],
                                   changes["held_all_up"]["deadline_monotonic_ns"])

    def test_lost_misordered_or_extra_prefix_frames_and_a_missing_barrier_fail(self):
        order = "The CEC FIFO did not transmit all ten admitted presses in order"
        for fault, error, message in (("prefix", AssertionError, order),
                                      ("non-key prefix", AssertionError, order),
                                      ("recipient", AssertionError, order),
                                      ("misordered prefix", AssertionError, order),
                                      ("dropped press", AssertionError, order),
                                      ("missing barrier", TimeoutError, "No fresh physical BLE key press")):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                self.assert_fails(fixture, error, message)

    def test_released_holds_cannot_replay_and_streams_must_run_until_one_final_release(self):
        barrier = "A released navigation hold replayed after the fresh FIFO barrier"
        consumer = "The consumer stream continued after its final release"
        missing = "No fresh physical BLE key release"
        for fault, error, message in (
                ("capacity release", AssertionError, "exactly two source and two consumer releases"),
                ("capacity held replay", AssertionError, barrier),
                ("late barrier repeat", AssertionError, barrier),
                ("volume down stopped at barrier up", AssertionError,
                 "A held volume key stopped repeating before its final release"),
                ("up replay", TimeoutError, missing),
                ("source after release", AssertionError, "Source presses continued after their final admitted release"),
                ("failed up", AssertionError, consumer),
                ("volume after release", AssertionError, consumer),
                ("missing source release", TimeoutError, missing),
                ("missing volume release", TimeoutError, missing)):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                self.assert_fails(fixture, error, message)

    def test_any_fresh_fifo_drop_log_fails(self):
        dropped = "The CEC FIFO dropped a frame during the ten-key check"
        for log_fault, message in (("volume drop", dropped), ("status drop", dropped), ("release drop", dropped),
                                   ("other wire drop", dropped),
                                   ("unrecognized drop", "A CEC drop log has an unrecognized format")):
            with self.subTest(log_fault=log_fault), self.fixture(log_fault=log_fault) as fixture:
                self.assert_fails(fixture, AssertionError, message)
                # A recognized drop is recorded for diagnosis before the failure.
                self.assertEqual(fixture.bed.control_observations[-1]["checks"][-1].get("fifo_drops"),
                                 fixture.expected_drops or None)

    def test_late_consumer_presses_and_extra_releases_cannot_hide_in_fresh_input_or_slow_reads(self):
        old = "An old consumer press replayed outside its admitted stream"
        releases = "exactly two source and two consumer releases"
        for fault, message in (("select old consumer", old), ("mute old consumer", old),
                               ("select extra release", releases), ("log release", releases), ("log mute", releases),
                               ("final owner release", releases), ("final owner old consumer", releases)):
            with self.subTest(fault=fault), self.fixture(fault=fault) as fixture:
                self.assert_fails(fixture, AssertionError, message)


if __name__ == "__main__":
    unittest.main()
