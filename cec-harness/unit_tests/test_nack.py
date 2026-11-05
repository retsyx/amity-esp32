"""Recipient withdrawal must produce real absence and retain wire coverage."""

from contextlib import contextmanager
from queue import Empty, Queue
import threading
import time
from types import SimpleNamespace
import unittest

from amity_harness import AvrComponent, CecSession, DUT, PlaybackComponent, PowerState
from amity_harness.cec import CecError, Frame, TxResult
from amity_harness.cec_pin import PinFrame
from amity_harness.cec_pin import PinDecoder
from amity_harness.nack import MissingCECRecipient, nack_attempts
from amity_harness.topology import CecAddress


class MonitorBackend:
    def __init__(self, gpio):
        self.gpio, self.queue = gpio, Queue()
        self.active, self.monitor, self.closed = (), False, False
        self.cec_version = 6
        self.witness_mode, self.witness_frames, self.witness_transmits = "normal", [], []
        self.empty_reads = 0
        self.configurations = []

    def open(self):
        pass

    def configure(self, components):
        self.active = tuple(components)
        self.configurations.append(self.active)

    def transmit(self, frame):
        now = time.monotonic_ns()
        if self.monitor:
            raw, stamp = frame.raw, now
            is_witness = frame.opcode == 0x9f
            if is_witness:
                self.witness_transmits.append(frame)
                if self.witness_mode == "source":
                    raw = bytes([0x60, 0x9f])
                elif self.witness_mode == "destination":
                    raw = bytes([0x51, 0x9f])
                elif self.witness_mode == "opcode":
                    raw = bytes([0x50, 0x9e])
                elif self.witness_mode == "operands":
                    raw += b"\x01"
                elif self.witness_mode == "stale":
                    stamp = self.synchronized_ns
            observed = PinFrame.observed(raw, timestamp_ns=stamp,
                eom=tuple(i == len(raw)-1 for i in range(len(raw))),
                ack=(frame.destination == 15,) * len(raw), synchronized_ns=self.synchronized_ns)
            if is_witness:
                if self.witness_mode == "terminal":
                    if self.witness_frames:
                        self.queue.put(self.witness_frames[-1])
                    self.witness_frames.append(observed)
                elif self.witness_mode == "delayed":
                    self.witness_frames.append(observed)
                elif self.witness_mode != "missing":
                    self.queue.put(observed)
            else:
                self.queue.put(observed)
        if frame.opcode == 0x9f and self.witness_mode == "virtual":
            return TxResult(frame, 1, now, transport="virtual")
        return TxResult(frame, 1, now)

    def receive(self, timeout):
        time.sleep(min(timeout, .005))
        return None

    def open_monitor(self):
        self.monitor = True
        self.synchronized_ns = time.monotonic_ns()

    def receive_monitor(self, timeout):
        try:
            result = self.queue.get(timeout=timeout)
        except Empty:
            self.empty_reads += 1
            return None
        if isinstance(result, BaseException):
            raise result
        return result

    def close_monitor(self):
        self.monitor = False

    def close(self):
        self.close_monitor();self.closed = True


class MissingRecipientTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        dut = DUT("amity-dut")
        dut._runtime = SimpleNamespace(cec_timeout=.08)
        avr = AvrComponent(dut.tentacle(0).back, "Failure AVR", address=CecAddress(5, 0x1000), power=PowerState.ON)
        player = PlaybackComponent(avr, "Failure source", port=1, power=PowerState.ON)
        with CecSession((avr, player), backend_factory=MonitorBackend) as session:
            bed = SimpleNamespace(_entered=True, _closed=False, session=session, duts=(dut,))
            dut._testbed = bed
            scan = SimpleNamespace(dut=dut, _session=session, _testbed=bed, expected=(avr, player),
                                   owner_boots={dut.identity.device_id: "a" * 32},
                                   owner_duts={dut.identity.device_id: dut}, initiators={player.gpio: 0},
                                   validate_owners=lambda components: None)
            dut.last_scan = scan
            yield dut, avr, player, scan, session._backends[player.gpio]

    @staticmethod
    def wait(predicate):
        deadline = time.monotonic() + 1
        while not predicate():
            if time.monotonic() >= deadline:
                raise TimeoutError("Scripted monitor worker did not respond")
            time.sleep(.002)

    def test_real_address_withdrawal_restore_and_original_header_evidence(self):
        with self.fixture() as (_, avr, player, scan, backend):
            with MissingCECRecipient(player, scan=scan) as missing:
                self.assertEqual(player.power, PowerState.OFF)
                self.assertEqual(backend.active, (avr,))
                self.assertTrue(backend.monitor)
                frame = PinFrame.observed(bytes([player.address.logical]), timestamp_ns=time.monotonic_ns(), eom=(False,), ack=(True,), synchronized_ns=backend.synchronized_ns)
                backend.queue.put(frame)
                self.wait(lambda: frame in missing.frames)
                self.assertIn(frame, missing.frames)
                missing.drain(time.monotonic_ns())
                self.assertEqual(len(backend.witness_transmits), 2)
                proof = scan._testbed.control_observations[0]["checks"][0]
                self.assertEqual(proof["frame"], "50:9f")
                self.assertGreaterEqual(proof["wire_timestamp_ns"], proof["requested_boundary_monotonic_ns"])
                restored = missing.restore()
                self.assertEqual(player.power, PowerState.ON)
                self.assertEqual(backend.active, (avr, player))
                self.assertTrue(backend.monitor)
                self.assertEqual(missing.restore(), restored)
            self.assertFalse(backend.monitor)
            self.assertFalse(backend.closed)
            with self.assertRaisesRegex(AssertionError, "not active"):
                _ = missing.frames

    def test_monitor_precedes_hold_and_retains_later_byte_nack_started_before_withdrawal(self):
        with self.fixture() as (_, avr, player, scan, backend):
            with MissingCECRecipient(player, scan=scan, withdraw_on_enter=False) as missing:
                self.assertEqual(player.power, PowerState.ON)
                self.assertEqual(backend.active, (avr, player))
                header = player.address.logical
                success = PinFrame.observed(bytes([header, 0x44, 4]), timestamp_ns=time.monotonic_ns(),
                    eom=(False, False, True), ack=(False, False, False),
                    synchronized_ns=backend.synchronized_ns)
                backend.queue.put(success)
                self.wait(lambda: success in missing.frames)
                withdrawn = missing.withdraw()
                self.assertEqual(player.power, PowerState.OFF)
                count = len(backend.configurations)
                self.assertEqual(missing.withdraw(), withdrawn)
                self.assertEqual(len(backend.configurations), count)
                # Its header started while still claimed. The original opcode
                # then arrived and NACKed after address withdrawal.
                failure = PinFrame.observed(bytes([header, 0x44]), timestamp_ns=withdrawn - 1,
                    eom=(False, False), ack=(False, True), synchronized_ns=backend.synchronized_ns)
                backend.queue.put(failure)
                self.wait(lambda: failure in missing.frames)
                self.assertGreater(failure.timestamp_ns, success.timestamp_ns)
                self.assertLess(failure.timestamp_ns, missing.withdrawn_ns)
                self.assertEqual(nack_attempts(missing.frames, header), (failure,))
                missing.drain(time.monotonic_ns())
            self.assertEqual(player.power, PowerState.ON)
            self.assertFalse(backend.monitor)

    def test_unused_monitor_context_and_restore_do_not_reconfigure_power(self):
        for primary in (False, True):
            with self.subTest(primary=primary), self.fixture() as (_, _, player, scan, backend):
                count = len(backend.configurations)
                try:
                    with MissingCECRecipient(player, scan=scan, withdraw_on_enter=False) as missing:
                        self.assertTrue(backend.monitor)
                        self.assertIsNone(missing.restore())
                        with self.assertRaisesRegex(RuntimeError, "Withdraw the recipient"):
                            missing.drain(time.monotonic_ns())
                        if primary:
                            raise ValueError("primary")
                except ValueError as error:
                    self.assertTrue(primary)
                    self.assertEqual(str(error), "primary")
                self.assertEqual(player.power, PowerState.ON)
                self.assertEqual(len(backend.configurations), count)
                self.assertFalse(backend.monitor)

    def test_monitor_thread_start_failure_closes_without_withdrawing(self):
        from unittest.mock import patch
        with self.fixture() as (_, _, player, scan, backend), \
             patch("amity_harness.nack.threading.Thread.start", side_effect=RuntimeError("thread start")):
            count = len(backend.configurations)
            with self.assertRaisesRegex(RuntimeError, "thread start"):
                MissingCECRecipient(player, scan=scan, withdraw_on_enter=False).__enter__()
            self.assertEqual(player.power, PowerState.ON)
            self.assertEqual(len(backend.configurations), count)
            self.assertFalse(backend.monitor)

    def test_second_actual_request_finalizes_first_witness_without_a_dut_reply(self):
        with self.fixture() as (_, _, player, scan, backend):
            backend.witness_mode = "terminal"
            with MissingCECRecipient(player, scan=scan) as missing:
                through = time.monotonic_ns()
                witnessed = missing.drain(through)
                self.assertEqual(len(backend.witness_transmits), 2)
                self.assertEqual(witnessed, backend.witness_frames[0].timestamp_ns)
                self.assertIn(backend.witness_frames[0], missing.frames)
                self.assertNotIn(backend.witness_frames[1], missing.frames)

    def test_empty_reads_cannot_certify_delayed_upstream_frames(self):
        with self.fixture() as (_, _, player, scan, backend):
            backend.witness_mode = "delayed"
            with MissingCECRecipient(player, scan=scan) as missing:
                through = time.monotonic_ns()
                delayed = PinFrame.observed(b"\x04", timestamp_ns=through, eom=(False,), ack=(True,),
                                            synchronized_ns=backend.synchronized_ns)
                result, completed = {}, threading.Event()

                def drain():
                    try:
                        result["witnessed"] = missing.drain(through)
                    except Exception as error:
                        result["error"] = error
                    finally:
                        completed.set()

                worker = threading.Thread(target=drain)
                worker.start()
                try:
                    self.wait(lambda: len(backend.witness_transmits) == 2 and backend.empty_reads > 0)
                    self.assertFalse(completed.is_set())
                    backend.queue.put(delayed)
                    backend.queue.put(backend.witness_frames[0])
                    self.assertTrue(completed.wait(1))
                    self.assertNotIn("error", result)
                    self.assertGreaterEqual(result["witnessed"], through)
                    self.assertLess(missing.frames.index(delayed), missing.frames.index(backend.witness_frames[0]))
                finally:
                    worker.join(timeout=1)

    def test_missing_wrong_or_stale_witness_cannot_certify_coverage(self):
        for mode in ("missing", "source", "destination", "opcode", "operands", "stale"):
            with self.subTest(mode=mode), self.fixture() as (_, _, player, scan, backend):
                backend.witness_mode = mode
                with MissingCECRecipient(player, scan=scan) as missing:
                    with self.assertRaisesRegex(TimeoutError, "exact physical CEC drain witness"):
                        missing.drain(time.monotonic_ns())
                    self.assertEqual(len(backend.witness_transmits), 2)

    def test_virtual_transmission_cannot_replace_the_physical_witness(self):
        with self.fixture() as (_, _, player, scan, backend):
            backend.witness_mode = "virtual"
            with MissingCECRecipient(player, scan=scan) as missing:
                with self.assertRaisesRegex(AssertionError, "exact physical CEC query"):
                    missing.drain(time.monotonic_ns())
                self.assertEqual(len(backend.witness_transmits), 1)

    def test_future_boundary_is_reached_before_actual_witness_submission(self):
        with self.fixture() as (_, _, player, scan, backend):
            with MissingCECRecipient(player, scan=scan) as missing:
                through = time.monotonic_ns() + 10_000_000
                witnessed = missing.drain(through)
                self.assertGreaterEqual(witnessed, through)
                self.assertEqual(len(backend.witness_transmits), 2)

    def test_dut_initiator_must_not_be_a_fixture_virtual_recipient(self):
        with self.fixture() as (_, avr, player, scan, backend):
            scan.initiators[player.gpio] = avr.address.logical
            with self.assertRaisesRegex((ValueError, AssertionError), "DUT initiator"):
                MissingCECRecipient(player, scan=scan).__enter__()
            self.assertEqual(player.power, PowerState.ON)
            self.assertFalse(backend.monitor)

    def test_monitor_errors_and_invalid_timestamps_fail_and_restore(self):
        for result in (OSError("receive failed"), Frame(0, 4, 0x44, b"\x04", timestamp_ns=0)):
            with self.subTest(result=result), self.fixture() as (_, _, player, scan, backend):
                with self.assertRaises(CecError):
                    with MissingCECRecipient(player, scan=scan) as missing:
                        backend.queue.put(result)
                        self.wait(lambda: missing._error is not None)
                        missing.assert_healthy()
                self.assertEqual(player.power, PowerState.ON)
                self.assertFalse(backend.monitor)

    def test_lost_owner_scope_fails_but_cleanup_still_restores(self):
        with self.fixture() as (dut, _, player, scan, backend):
            with MissingCECRecipient(player, scan=scan) as missing:
                scan.owner_boots[dut.identity.device_id] = "b" * 32
                with self.assertRaisesRegex(AssertionError, "original owner"):
                    missing.assert_healthy()
            self.assertEqual(player.power, PowerState.ON)
            self.assertFalse(backend.monitor)

    def test_acquisition_must_precede_withdrawal(self):
        with self.fixture() as (_, _, player, scan, backend):
            with MissingCECRecipient(player, scan=scan) as missing:
                self.wait(lambda: missing.coverage_started_ns > 0)
                missing.withdrawn_ns = missing.coverage_started_ns - 1
                with self.assertRaisesRegex(AssertionError, "synchronize before"):
                    missing.drain(time.monotonic_ns())
            self.assertEqual(player.power, PowerState.ON)
            self.assertFalse(backend.monitor)

    def test_changed_acquisition_boundary_invalidates_observation_and_restores(self):
        with self.fixture() as (_, _, player, scan, backend):
            with self.assertRaises(CecError):
                with MissingCECRecipient(player, scan=scan) as missing:
                    self.wait(lambda: missing.coverage_started_ns > 0)
                    backend.queue.put(PinFrame.observed(b"\x04", timestamp_ns=time.monotonic_ns(),
                                      eom=(False,), ack=(True,),
                                      synchronized_ns=backend.synchronized_ns + 1))
                    self.wait(lambda: missing._error is not None)
                    missing.assert_healthy()
            self.assertEqual(player.power, PowerState.ON)
            self.assertFalse(backend.monitor)

    def test_missing_witness_or_monitor_capability_cannot_withdraw_address(self):
        for absent in ("witness", "monitor"):
            with self.subTest(absent=absent), self.fixture() as (_, avr, player, scan, backend):
                if absent == "witness":
                    avr.power_off()
                else:
                    backend.open_monitor = lambda: (_ for _ in ()).throw(CecError("Unsupported monitor"))
                with self.assertRaises((ValueError, CecError)):
                    MissingCECRecipient(player, scan=scan).__enter__()
                self.assertEqual(player.power, PowerState.ON)
                self.assertFalse(backend.monitor)

    def test_primary_failure_closes_monitor_and_restores_recipient(self):
        with self.fixture() as (_, _, player, scan, backend):
            with self.assertRaisesRegex(ValueError, "primary"):
                with MissingCECRecipient(player, scan=scan):
                    raise ValueError("primary")
            self.assertEqual(player.power, PowerState.ON)
            self.assertFalse(backend.monitor)

    def test_pin_failure_is_logged_as_safe_fields_and_retained_on_wrapped_error(self):
        from unittest.mock import patch
        try:
            PinDecoder().feed(3, time.monotonic_ns(), flags=2)
        except CecError as error:
            original = error
        with self.fixture() as (_, _, player, scan, backend), \
             patch("amity_harness.nack.event") as logged:
            with self.assertRaises(CecError) as failure:
                with MissingCECRecipient(player, scan=scan) as missing:
                    backend.queue.put(original)
                    self.wait(lambda: missing._error is not None)
                    missing.assert_healthy()
            self.assertEqual(failure.exception.pin_diagnostic, original.pin_diagnostic)
            rows = [call for call in logged.call_args_list if call.args == ("cec.pin.coverage_failed",)]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0].kwargs["diagnostic"]["reason"], "dropped pin events")
            self.assertNotIn("error", rows[0].kwargs)
            self.assertEqual(player.power, PowerState.ON)

    def test_withdrawal_error_survives_cleanup_failure(self):
        with self.fixture() as (_, _, player, scan, backend):
            primary = CecError("original withdrawal failure")
            player.power_off = lambda: (_ for _ in ()).throw(primary)
            player.set_power = lambda value: (_ for _ in ()).throw(OSError("restoration failure"))
            with self.assertRaises(CecError) as failure:
                MissingCECRecipient(player, scan=scan).__enter__()
            self.assertIs(failure.exception, primary)
            self.assertFalse(backend.monitor)
            self.assertTrue(any("OSError" in note for note in getattr(primary, "__notes__", ())))


class NackAttemptTests(unittest.TestCase):
    def observed(self, raw, eom, ack):
        return PinFrame.observed(raw, timestamp_ns=100, synchronized_ns=1, eom=eom, ack=ack)

    def test_every_original_partial_key_shape_is_retained_without_inventing_bytes(self):
        shapes = ((b"\x04", (False,), (True,)),
                  (b"\x04\x44", (False, False), (False, True)),
                  (b"\x04\x44\x04", (False, False, True), (False, False, True)),
                  (b"\x04\x45", (False, True), (False, True)))
        frames = tuple(self.observed(*shape) for shape in shapes)
        self.assertEqual(nack_attempts(frames, 4), frames)
        self.assertIsNone(frames[0].opcode)
        self.assertEqual(frames[1].operands, b"")
        self.assertEqual(frames[2].operands, b"\x04")
        self.assertEqual(frames[3].operands, b"")

    def test_successes_polls_and_other_recipients_are_not_failed_key_attempts(self):
        frames = (self.observed(b"\x04\x44\x04", (False, False, True), (False, False, False)),
                  self.observed(b"\x04\x45", (False, True), (False, False)),
                  self.observed(b"\x04\x8f", (False, True), (False, False)),
                  self.observed(b"\x04", (True,), (False,)),
                  self.observed(b"\x05", (False,), (True,)))
        self.assertEqual(nack_attempts(frames, 4), ())

    def test_wrong_eom_extra_operands_polls_and_unknown_nack_opcodes_fail_closed(self):
        shapes = ((b"\x04", (True,), (True,)),
                  (b"\x04\x44", (False, True), (False, True)),
                  (b"\x04\x44\x04", (False, False, False), (False, False, True)),
                  (b"\x04\x45", (False, False), (False, True)),
                  (b"\x04\x45\x04", (False, False, True), (False, False, True)),
                  (b"\x04\x44\x04\x01", (False, False, False, True), (False, False, False, True)),
                  (b"\x04\x83", (False, True), (False, True)))
        for shape in shapes:
            with self.subTest(raw=shape[0]), self.assertRaises(AssertionError):
                nack_attempts((self.observed(*shape),), 4)

    def test_nonphysical_or_corrupted_ack_slots_and_broadcast_header_fail_closed(self):
        frame = self.observed(b"\x04\x44", (False, False), (False, True))
        for ack in ((True, False), (False,), (0, True)):
            with self.subTest(ack=ack):
                changed = self.observed(b"\x04\x44", (False, False), (False, True))
                object.__setattr__(changed, "ack", ack)
                with self.assertRaises(AssertionError):
                    nack_attempts((changed,), 4)
        with self.assertRaises(AssertionError):
            nack_attempts((Frame(0, 4),), 4)
        for header in (True, -1, 256, 15, 0xf4):
            with self.subTest(header=header), self.assertRaises(ValueError):
                nack_attempts((frame,), header)


if __name__ == "__main__":
    unittest.main()
