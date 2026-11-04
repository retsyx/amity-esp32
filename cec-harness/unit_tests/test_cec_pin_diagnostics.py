"""Pin failures retain bounded physical timing facts, never arbitrary text."""

from collections import deque
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.cec import CecError
from amity_harness.cec_pin import (PinCoverageError, PinDecoder, PinMonitor,
                                  pin_diagnostic, pin_monitor_error, validate_pin_diagnostic)
from unit_tests.test_cec_pin import waveform


class PinDiagnosticTests(unittest.TestCase):
    def malformed_pulse(self):
        events, _ = waveform(b"\x04")
        events[3].ts = events[2].ts + 1_000_000
        decoder = PinDecoder()
        with self.assertRaises(PinCoverageError) as failed:
            for notice in events[:4]:
                decoder.feed(notice.event, notice.ts, flags=notice.flags)
        return failed.exception, events[:4]

    def test_malformed_pulse_retains_exact_phase_and_original_edge_deltas(self):
        error, events = self.malformed_pulse()
        self.assertIsInstance(error, CecError)
        self.assertEqual(error.pin_diagnostic, {
            "reason": "malformed data pulse", "phase": "data", "bit_index": 0,
            "decoded_bytes": 0, "level": True, "previous_level": False,
            "terminal": False, "synchronized_ns": events[0].ts,
            "frame_start_ns": events[0].ts, "last_event_ns": events[-1].ts,
            "fall_ns": events[2].ts, "fault_ns": events[-1].ts,
            "edge_delta_ns": 1_000_000, "low_delta_ns": 1_000_000,
            "recent_events": [[notice.event, notice.ts, notice.flags] for notice in events],
        })

    def test_malformed_period_distinguishes_low_edge_from_previous_high(self):
        events, _ = waveform(b"\x04")
        events[4].ts = events[2].ts + 2_800_000
        decoder = PinDecoder()
        with self.assertRaises(PinCoverageError) as failed:
            for notice in events[:5]:
                decoder.feed(notice.event, notice.ts, flags=notice.flags)
        proof = failed.exception.pin_diagnostic
        self.assertEqual(proof["reason"], "malformed data period")
        self.assertEqual((proof["phase"], proof["bit_index"], proof["decoded_bytes"]), ("data", 1, 0))
        self.assertIs(proof["level"], False)
        self.assertIs(proof["previous_level"], True)
        self.assertEqual(proof["fall_ns"], events[2].ts)
        self.assertEqual(proof["fault_ns"], events[4].ts)
        self.assertEqual(proof["edge_delta_ns"], 1_300_000)
        self.assertEqual(proof["low_delta_ns"], 2_800_000)

    def test_later_edge_failure_keeps_its_original_phase_and_fault_time(self):
        events, end = waveform(b"\x04")
        decoder = PinDecoder()
        for notice in events[:16]:
            decoder.feed(notice.event, notice.ts, flags=notice.flags)
        # Only the malformed measured low is a failure; no clock observation
        # participates in classifying these original pin events.
        decoder.feed(events[16].event, events[16].ts)
        fault = events[16].ts + 1_000_000
        with self.assertRaises(PinCoverageError) as failed:
            decoder.feed(4, fault)
        proof = failed.exception.pin_diagnostic
        self.assertEqual(proof["reason"], "malformed data pulse")
        self.assertEqual(proof["fault_ns"], fault)
        self.assertEqual(proof["last_event_ns"], fault)
        self.assertEqual(proof["edge_delta_ns"], 1_000_000)
        self.assertEqual(proof["low_delta_ns"], 1_000_000)
        self.assertEqual(proof["recent_events"][-1], [4, fault, 0])

    def test_recent_kernel_history_is_bounded_and_includes_the_fault_event(self):
        decoder = PinDecoder()
        for index in range(1, 61):
            decoder.feed(1, index * 1_000)
        with self.assertRaises(PinCoverageError) as failed:
            decoder.feed(2, 61_000)
        proof = failed.exception.pin_diagnostic
        self.assertEqual(proof["reason"], "lost kernel messages")
        self.assertEqual(len(proof["recent_events"]), 32)
        self.assertEqual(proof["recent_events"][0], [1, 30_000, 0])
        self.assertEqual(proof["recent_events"][-1], [2, 61_000, 0])

    def test_validation_copies_the_complete_schema_and_every_history_row(self):
        error, _ = self.malformed_pulse()
        original = deepcopy(error.pin_diagnostic)
        copied = validate_pin_diagnostic(original)
        self.assertEqual(copied, original)
        self.assertIsNot(copied, original)
        self.assertIsNot(copied["recent_events"], original["recent_events"])
        self.assertIsNot(copied["recent_events"][0], original["recent_events"][0])
        original["recent_events"][0][1] = 0
        self.assertNotEqual(copied["recent_events"][0][1], 0)
        original["recent_events"] = tuple(tuple(row) for row in copied["recent_events"])
        self.assertEqual(validate_pin_diagnostic(original)["recent_events"], copied["recent_events"])

    def test_unknown_keys_missing_fields_or_arbitrary_reason_are_rejected(self):
        error, _ = self.malformed_pulse()
        candidates = []
        value = deepcopy(error.pin_diagnostic);value["admin_password"] = "PRIVATE_TOKEN";candidates.append(value)
        value = deepcopy(error.pin_diagnostic);del value["phase"];candidates.append(value)
        value = deepcopy(error.pin_diagnostic);value["reason"] = "PRIVATE_TOKEN";candidates.append(value)
        value = deepcopy(error.pin_diagnostic);value["phase"] = "PRIVATE_TOKEN";candidates.append(value)
        for value in (*candidates, None, [], "PRIVATE_TOKEN"):
            with self.subTest(value_type=type(value).__name__), self.assertRaises(ValueError) as failed:
                validate_pin_diagnostic(value)
            self.assertNotIn("PRIVATE_TOKEN", str(failed.exception))

    def test_invalid_positions_types_levels_and_numeric_ranges_are_rejected(self):
        error, _ = self.malformed_pulse()
        changes = (("bit_index", 11), ("bit_index", -1), ("decoded_bytes", 17),
                   ("fault_ns", True), ("fall_ns", "PRIVATE_TOKEN"),
                   ("edge_delta_ns", 1.0), ("last_event_ns", None),
                   ("fault_ns", 2**64), ("fault_ns", -(2**64)),
                   ("level", 1), ("previous_level", 0), ("terminal", None),
                   ("terminal", 1), ("reason", {}), ("phase", []))
        for field, replacement in changes:
            with self.subTest(field=field, replacement=replacement), self.assertRaises(ValueError):
                value = deepcopy(error.pin_diagnostic)
                value[field] = replacement
                validate_pin_diagnostic(value)

    def test_oversized_or_malformed_event_history_is_rejected(self):
        error, _ = self.malformed_pulse()
        histories = ([[3, 1, 0]] * 33, [[3, 1, 0, 0]], [[9, 1, 0]],
                     [[3, -1, 0]], [[3, 2**64, 0]], [[3, 1, -1]],
                     [[3, 1, 2**32]], [[True, 1, 0]], [[3, "PRIVATE_TOKEN", 0]],
                     ["PRIVATE_TOKEN"], {}, None)
        for history in histories:
            with self.subTest(history_type=type(history).__name__), self.assertRaises(ValueError):
                value = deepcopy(error.pin_diagnostic)
                value["recent_events"] = history
                validate_pin_diagnostic(value)

    def test_monitor_receive_and_close_keep_known_pin_facts(self):
        error, _ = self.malformed_pulse()
        monitor = object.__new__(PinMonitor)
        monitor._condition = threading.Condition()
        monitor._stop = threading.Event()
        monitor._error, monitor._frames = error, deque()
        monitor._thread = SimpleNamespace(join=lambda **kwargs: None, is_alive=lambda: False)
        for operation in (lambda: monitor.receive(0), monitor.close):
            with self.subTest(operation=operation), self.assertRaises(CecError) as failed:
                operation()
            self.assertEqual(failed.exception.pin_diagnostic, error.pin_diagnostic)
            self.assertIsNot(failed.exception.pin_diagnostic, error.pin_diagnostic)

    def test_wrapping_unknown_exception_text_or_invalid_proof_exposes_neither(self):
        error, _ = self.malformed_pulse()
        invalid = RuntimeError("PRIVATE_TOKEN")
        invalid.pin_diagnostic = {**error.pin_diagnostic, "password": "PRIVATE_TOKEN"}
        for original in (RuntimeError("PRIVATE_TOKEN"), invalid):
            self.assertIsNone(pin_diagnostic(original))
            wrapped = pin_monitor_error(original)
            self.assertIsNone(getattr(wrapped, "pin_diagnostic", None))
            self.assertNotIn("PRIVATE_TOKEN", str(wrapped))


if __name__ == "__main__":
    unittest.main()
