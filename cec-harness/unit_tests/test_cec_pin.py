"""Decode real CEC pin waveforms without inventing aborted message bytes."""

from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.cec import CecError
from amity_harness.cec_pin import PinDecoder


def edge(timestamp, high, *, flags=0):
    return SimpleNamespace(ts=timestamp, event=4 if high else 3, flags=flags)


def waveform(raw, *, started=1_000_000_000, eom=None, ack=None,
             start_low=3_700_000, start_period=4_500_000,
             one_low=600_000, zero_low=1_500_000, bit_period=2_400_000):
    """Physical 8-bit/MSB-first bytes, each followed by EOM and ACK slots."""
    eom = tuple(index == len(raw) - 1 for index in range(len(raw))) if eom is None else tuple(eom)
    ack = (False,) * len(raw) if ack is None else tuple(ack)
    assert len(eom) == len(ack) == len(raw)
    events = [edge(started, False), edge(started + start_low, True)]
    cursor = started + start_period
    for byte, last, nack in zip(raw, eom, ack):
        bits = [bool(byte & (1 << shift)) for shift in range(7, -1, -1)]
        for value in (*bits, last, nack):
            events.extend((edge(cursor, False), edge(cursor + (one_low if value else zero_low), True)))
            cursor += bit_period
    return events, cursor


class CECPinDecoderTests(unittest.TestCase):
    def feed(self, decoder, events):
        frames = []
        for notice in events:
            frames.extend(decoder.feed(notice.event, notice.ts, flags=notice.flags))
        return frames

    def decode(self, decoder, events, through):
        # The final falling edge is actual next-frame input, never a wall-clock
        # inference. Leave it in the decoder as that next frame's start.
        return self.feed(decoder, [*events, edge(through, False)])

    def test_destination_nack_preserves_the_actual_non_poll_header_only(self):
        events, end = waveform(b"\x04", eom=(False,), ack=(True,))
        decoder = PinDecoder()
        frames = self.decode(decoder, events, end)
        self.assertEqual(len(frames), 1)
        frame = frames[0]
        self.assertEqual(frame.raw, b"\x04")
        self.assertEqual((frame.source, frame.destination, frame.opcode, frame.operands), (0, 4, None, b""))
        self.assertEqual(frame.eom, (False,))
        self.assertEqual(frame.ack, (True,))
        self.assertEqual(frame.started_ns, events[0].ts)
        self.assertEqual(frame.timestamp_ns, events[0].ts)
        self.assertEqual(decoder.synchronized_ns, events[0].ts)
        self.assertEqual(frame.synchronized_ns, events[0].ts)

    def test_ordinary_poll_retains_eom_and_cannot_look_like_an_aborted_key(self):
        for nack in (False, True):
            with self.subTest(nack=nack):
                events, end = waveform(b"\x04", eom=(True,), ack=(nack,))
                frames = self.decode(PinDecoder(), events, end)
                self.assertEqual(len(frames), 1)
                self.assertEqual(frames[0].raw, b"\x04")
                self.assertEqual(frames[0].eom, (True,))
                self.assertEqual(frames[0].ack, (nack,))

    def test_acknowledged_header_continues_to_the_complete_key_frame(self):
        events, end = waveform(b"\x04\x44\x04")
        decoder = PinDecoder()
        self.assertEqual(self.feed(decoder, events[:22]), [])
        frames = self.decode(decoder, events[22:], end)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].raw, b"\x04\x44\x04")
        self.assertEqual(frames[0].eom, (False, False, True))
        self.assertEqual(frames[0].ack, (False, False, False))

    def test_broadcast_high_ack_slots_do_not_abort_each_byte(self):
        events, end = waveform(b"\x0f\x82\x11\x00", ack=(True,) * 4)
        frames = self.decode(PinDecoder(), events, end)
        self.assertEqual([frame.raw for frame in frames], [b"\x0f\x82\x11\x00"])
        self.assertEqual(frames[0].ack, (True,) * 4)

    def test_release_and_next_frame_keep_separate_start_timestamps(self):
        first, end = waveform(b"\x04\x45")
        second, end = waveform(b"\x05\x44\x41", started=end + 16_800_000)
        frames = self.decode(PinDecoder(), first + second, end)
        self.assertEqual([frame.raw for frame in frames], [b"\x04\x45", b"\x05\x44\x41"])
        self.assertEqual([frame.started_ns for frame in frames], [first[0].ts, second[0].ts])

    def test_terminal_ack_is_not_finalized_before_its_valid_slot_end(self):
        events, end = waveform(b"\x04", eom=(False,), ack=(True,))
        decoder = PinDecoder()
        self.assertEqual(self.feed(decoder, events), [])
        ack_started = end - 2_400_000
        frames = decoder.feed(3, ack_started + 2_050_000)
        self.assertEqual([frame.raw for frame in frames], [b"\x04"])
        self.assertEqual(decoder.pending_start_ns, ack_started + 2_050_000)

    def test_next_falling_edge_both_finishes_ack_and_starts_next_frame(self):
        first, end = waveform(b"\x04", eom=(False,), ack=(True,))
        second, second_end = waveform(b"\x05\x45", started=end + 16_800_000)
        decoder = PinDecoder()
        self.assertEqual(self.feed(decoder, first), [])
        completed = decoder.feed(second[0].event, second[0].ts)
        self.assertEqual([frame.raw for frame in completed], [b"\x04"])
        self.assertEqual(decoder.pending_start_ns, second[0].ts)
        completed = self.decode(decoder, second[1:], second_end)
        self.assertEqual([frame.raw for frame in completed], [b"\x05\x45"])
        self.assertEqual(completed[0].timestamp_ns, second[0].ts)

    def test_actual_short_ack_period_remains_invalid(self):
        events, end = waveform(b"\x04", eom=(False,), ack=(True,))
        decoder = PinDecoder()
        self.feed(decoder, events)
        with self.assertRaises(CecError):
            decoder.feed(3, end - 2_400_000 + 2_049_999)

    def test_later_byte_nack_keeps_only_bytes_that_reached_the_wire(self):
        events, end = waveform(b"\x04\x44", eom=(False, False), ack=(False, True))
        frames = self.decode(PinDecoder(), events, end)
        self.assertEqual([frame.raw for frame in frames], [b"\x04\x44"])
        self.assertEqual(frames[0].operands, b"")
        self.assertEqual(frames[0].eom, (False, False))
        self.assertEqual(frames[0].ack, (False, True))

    def test_initial_mid_frame_edges_are_ignored_until_a_valid_start(self):
        middle = [edge(10_000_000 + index * 1_200_000, bool(index % 2)) for index in range(12)]
        complete, end = waveform(b"\x04\x44\x04", started=50_000_000)
        frames = self.decode(PinDecoder(), middle + complete, end)
        self.assertEqual([frame.raw for frame in frames], [b"\x04\x44\x04"])

    def test_initial_pin_state_is_acquisition_only_and_metadata_loss_is_separate(self):
        decoder = PinDecoder()
        self.assertEqual(decoder.feed(4, 1_000_000, flags=1), ())
        self.assertEqual(decoder.feed(1, 2_000_000, flags=2), ())
        events, end = waveform(b"\x04\x45")
        self.assertEqual([frame.raw for frame in self.decode(decoder, events, end)], [b"\x04\x45"])
        with self.assertRaises(CecError):
            decoder.feed(4, end + 1_000_000, flags=1)

    def test_initial_low_level_is_not_an_observed_start_falling_edge(self):
        partial, end = waveform(b"\x04", eom=(False,), ack=(True,))
        partial[0].flags = 1
        decoder = PinDecoder()
        self.assertEqual(self.decode(decoder, partial, end), [])
        self.assertEqual(decoder.synchronized_ns, 0)
        complete, end = waveform(b"\x04\x45", started=end + 16_800_000)
        self.assertEqual([frame.raw for frame in self.decode(decoder, complete, end)], [b"\x04\x45"])

    def test_pin_loss_before_acquisition_or_message_loss_is_fatal(self):
        for kind in (2, 3, 4):
            with self.subTest(kind=kind), self.assertRaises(CecError):
                PinDecoder().feed(kind, 1_000_000, flags=2 if kind != 2 else 0)

    def test_a_synchronized_invalid_low_pulse_cannot_be_silently_reacquired(self):
        events, end = waveform(b"\x04\x45")
        decoder = PinDecoder()
        self.feed(decoder, events)
        with self.assertRaises(CecError):
            self.feed(decoder, [edge(end + 10_000_000, False), edge(end + 11_000_000, True)])

    def test_valid_pulse_and_period_limits_keep_bit_values_unambiguous(self):
        for timings in ({"start_low": 3_500_000, "start_period": 4_300_000,
                         "one_low": 400_000, "zero_low": 1_300_000, "bit_period": 2_050_000},
                        {"start_low": 3_900_000, "start_period": 4_700_000,
                         "one_low": 800_000, "zero_low": 1_700_000, "bit_period": 2_750_000}):
            with self.subTest(timings=timings):
                events, end = waveform(b"\x04\x44\x04", **timings)
                self.assertEqual([frame.raw for frame in self.decode(PinDecoder(), events, end)], [b"\x04\x44\x04"])

    def test_invalid_data_pulses_or_periods_after_sync_fail_coverage(self):
        for field, value in (("one_low", 300_000), ("one_low", 1_000_000),
                             ("zero_low", 1_800_000), ("bit_period", 2_000_000),
                             ("bit_period", 2_800_000), ("start_period", 4_200_000),
                             ("start_period", 4_800_000)):
            with self.subTest(field=field, value=value), self.assertRaises(CecError):
                prefix, end = waveform(b"\x04\x45")
                decoder = PinDecoder()
                self.feed(decoder, prefix)
                events, _ = waveform(b"\x04\x44\x04", started=end + 16_800_000, **{field: value})
                self.feed(decoder, events)

    def test_new_start_cannot_hide_a_synchronized_truncated_frame(self):
        events, end = waveform(b"\x04", eom=(False,), ack=(False,))
        next_frame, _ = waveform(b"\x04\x44\x04", started=end + 16_800_000)
        with self.assertRaises(CecError):
            self.feed(PinDecoder(), events + next_frame)

    def test_incomplete_byte_or_missing_ack_cannot_become_a_header(self):
        for keep in (2 + 2 * 7, 2 + 2 * 8, 2 + 2 * 9):
            with self.subTest(events=keep):
                events, end = waveform(b"\x04", eom=(False,), ack=(True,))
                decoder = PinDecoder()
                self.assertEqual(self.feed(decoder, events[:keep]), [])
                with self.assertRaises(CecError):
                    decoder.feed(3, end + 10_000_000)

    def test_dropped_regressing_and_duplicate_edges_cannot_pass(self):
        for fault in ("dropped", "regressed", "duplicate"):
            with self.subTest(fault=fault):
                events, _ = waveform(b"\x04", eom=(False,), ack=(True,))
                if fault == "dropped":
                    events[8].flags = 2
                elif fault == "regressed":
                    events[8].ts = events[7].ts - 1
                else:
                    events[8].event = events[7].event
                with self.assertRaises(CecError):
                    self.feed(PinDecoder(), events)

    def test_frame_byte_bound_does_not_overflow_or_truncate_into_valid_evidence(self):
        raw = bytes([0x04, *range(1, 16)])
        events, end = waveform(raw)
        self.assertEqual([frame.raw for frame in self.decode(PinDecoder(), events, end)], [raw])
        events, end = waveform(raw + b"\x10")
        with self.assertRaises(CecError):
            self.decode(PinDecoder(), events, end)


if __name__ == "__main__":
    unittest.main()
