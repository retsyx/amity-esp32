"""Positive pin timestamps must be fresh, exact and fenced to the scanned wire."""

from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.cec import CecError, Frame
from amity_harness.cec_pin import PinDecoder, PinFrame
from amity_harness.pin_observation import PinObservation
from unit_tests import test_nack


class PinObservationTests(unittest.TestCase):
    def fixture(self):
        return test_nack.MissingRecipientTests().fixture()

    def frame(self, backend, raw=b"\x04\x44\x00", *, stamp=None, eom=None, ack=None, sync=None):
        return PinFrame.observed(raw, timestamp_ns=time.monotonic_ns() if stamp is None else stamp,
            eom=tuple(index == len(raw) - 1 for index in range(len(raw))) if eom is None else eom,
            ack=(False,) * len(raw) if ack is None else ack,
            synchronized_ns=backend.synchronized_ns if sync is None else sync)

    def wait(self, predicate):
        test_nack.MissingRecipientTests.wait(predicate)

    def test_exact_positive_start_is_preserved_without_changing_addresses_or_power(self):
        with self.fixture() as (_, avr, source, scan, backend):
            power, active = source.power, backend.active
            backend.clock_uncertainty_ns = 1_000
            with PinObservation(source, scan=scan) as pins:
                self.assertTrue(backend.monitor)
                after = time.monotonic_ns()
                frame = self.frame(backend)
                backend.queue.put(frame)
                found = pins.first_frame(frame.raw, after=after)
                self.assertIs(found, frame)
                self.assertEqual(found.timestamp_ns, frame.started_ns)
                self.assertEqual(pins.clock_uncertainty_ns, 1_000)
                self.assertEqual(source.power, power)
                self.assertEqual(backend.active, active)
            self.assertFalse(backend.monitor)
            self.assertFalse(backend.closed)
            self.assertEqual(backend.active, (avr, source))

    def test_wrong_stale_partial_or_nacked_frames_cannot_prove_the_requested_press(self):
        with self.fixture() as (_, _, source, scan, backend), PinObservation(source, scan=scan) as pins:
            after = time.monotonic_ns()
            for frame in (self.frame(backend, b"\x14\x44\x00"), self.frame(backend, b"\x05\x44\x00"),
                          self.frame(backend, b"\x04\x45"), self.frame(backend, b"\x04\x44\x01"),
                          self.frame(backend, stamp=after),
                          self.frame(backend, eom=(False, False, False), ack=(False, False, True)),
                          self.frame(backend, ack=(False, False, True))):
                backend.queue.put(frame)
            with self.assertRaisesRegex(TimeoutError, "exact fresh physical CEC start"):
                pins.first_frame(b"\x04\x44\x00", after=after, timeout=.015)
            correct = self.frame(backend)
            backend.queue.put(correct)
            self.assertIs(pins.first_frame(correct.raw, after=after), correct)

    def test_empty_monitor_reads_are_not_positive_evidence(self):
        with self.fixture() as (_, _, source, scan, backend), PinObservation(source, scan=scan) as pins:
            with self.assertRaises(TimeoutError):
                pins.first_frame(b"\x04\x44\x00", after=time.monotonic_ns(), timeout=.01)
            self.assertTrue(backend.monitor)

    def test_burst_checks_do_not_acquire_model_health_or_http_owner_fences(self):
        with self.fixture() as (_, _, source, scan, backend):
            original = scan.validate_owners
            with PinObservation(source, scan=scan) as pins:
                after = time.monotonic_ns()
                frame = self.frame(backend)
                backend.queue.put(frame)
                with patch.object(scan, "validate_owners", side_effect=AssertionError("No HTTP in burst")), \
                     patch.object(scan._session, "assert_healthy", side_effect=AssertionError("No model lock in burst")):
                    pins.assert_healthy()
                    self.assertIs(pins.first_frame(frame.raw, after=after), frame)
                self.assertIs(scan.validate_owners, original)

    def test_changed_owner_scan_wire_or_session_invalidates_positive_evidence(self):
        for fault in ("owner", "initiator", "scan", "bed", "session", "clock"):
            with self.subTest(fault=fault), self.fixture() as (dut, _, source, scan, backend):
                with self.assertRaises(AssertionError):
                    with PinObservation(source, scan=scan) as pins:
                        if fault == "owner":
                            scan.owner_boots[dut.identity.device_id] = "c" * 32
                        elif fault == "initiator":
                            scan.initiators[source.gpio] = 9
                        elif fault == "scan":
                            dut.last_scan = None
                        elif fault == "bed":
                            scan._testbed = None
                        elif fault == "session":
                            source._session = None
                        else:
                            backend.clock_uncertainty_ns = 1
                        pins.assert_healthy()
                self.assertFalse(backend.monitor)

    def test_monitor_errors_invalid_frames_and_resynchronization_fail_closed(self):
        for fault in ("transport", "frame", "synchronization"):
            with self.subTest(fault=fault), self.fixture() as (_, _, source, scan, backend):
                with self.assertRaises(CecError):
                    with PinObservation(source, scan=scan) as pins:
                        first = self.frame(backend)
                        backend.queue.put(first)
                        pins.first_frame(first.raw, after=pins.started_ns)
                        result = (OSError("private monitor failure") if fault == "transport" else
                                  Frame(0, 4, 0x44, b"\x00") if fault == "frame" else
                                  self.frame(backend, sync=backend.synchronized_ns + 1))
                        backend.queue.put(result)
                        self.wait(lambda: pins._error is not None)
                        pins.assert_healthy()
                self.assertFalse(backend.monitor)

    def test_observation_queue_is_bounded(self):
        with self.fixture() as (_, _, source, scan, backend):
            with self.assertRaises(CecError):
                with PinObservation(source, scan=scan) as pins:
                    for _ in range(4_097):
                        backend.queue.put(self.frame(backend))
                    self.wait(lambda: pins._error is not None)
                    pins.assert_healthy()
            self.assertFalse(backend.monitor)

    def test_full_owner_checks_run_after_the_burst_and_monitor_always_closes(self):
        with self.fixture() as (_, _, source, scan, backend):
            with self.assertRaisesRegex(AssertionError, "Owner changed after burst"):
                with PinObservation(source, scan=scan):
                    scan.validate_owners = lambda components: (_ for _ in ()).throw(AssertionError("Owner changed after burst"))
            self.assertFalse(backend.monitor)

    def test_primary_failure_is_preserved_even_if_monitor_cleanup_also_fails(self):
        with self.fixture() as (_, _, source, scan, backend):
            close = backend.close_monitor
            with self.assertRaisesRegex(ValueError, "primary") as failed:
                with PinObservation(source, scan=scan):
                    def broken_close():
                        close()
                        raise OSError("cleanup")
                    backend.close_monitor = broken_close
                    raise ValueError("primary")
            backend.close_monitor = close
            self.assertFalse(backend.monitor)
            self.assertTrue(any("OSError" in note for note in getattr(failed.exception, "__notes__", ())))

    def test_pin_fault_diagnostics_survive_the_safe_error_wrapper(self):
        try:
            PinDecoder().feed(3, time.monotonic_ns(), flags=2)
        except CecError as original:
            fault = original
        with self.fixture() as (_, _, source, scan, backend):
            with self.assertRaises(CecError) as failed:
                with PinObservation(source, scan=scan) as pins:
                    backend.queue.put(fault)
                    self.wait(lambda: pins._error is not None)
                    pins.assert_healthy()
            self.assertEqual(failed.exception.pin_diagnostic, fault.pin_diagnostic)


if __name__ == "__main__":
    unittest.main()
