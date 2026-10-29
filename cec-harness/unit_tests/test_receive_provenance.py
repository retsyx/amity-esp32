"""RX injection changes model handling while retaining observed wire evidence."""

from pathlib import Path
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecAddress, CecSession, DUT, Frame, Hooks, TxResult
from amity_harness.discovery import _fresh_discovery


class IdleBackend:
    """Unit-only wire: retain TX results without touching GPIO, SSH or a DUT."""

    def __init__(self, gpio):
        self.gpio = gpio
        self.idle = threading.Event()

    def open(self):
        pass

    def configure(self, components):
        pass

    def transmit(self, frame):
        return TxResult(frame, 1, time.monotonic_ns())

    def receive(self, timeout):
        self.idle.wait(timeout)
        return None

    def close(self):
        self.idle.set()


class ReceiveProvenanceTests(unittest.TestCase):
    def component(self):
        return AvrComponent(DUT("amity-dut"), "Provenance AVR",
                            address=CecAddress(5, "1.0.0.0"))

    def test_rx_injection_changes_model_but_preserves_observed_frame_and_time(self):
        avr = self.component()
        handled = []
        with CecSession([avr], backend_factory=IdleBackend) as session:
            arrived_ns = time.monotonic_ns()
            observed = Frame(0, 5, 0x8f, timestamp_ns=arrived_ns)
            injected = Frame(0, 5, 0x44, b"\x41", timestamp_ns=arrived_ns + 10**12)
            avr.hooks = Hooks(before_rx=lambda component, frame: injected,
                              after_rx=lambda component, frame: handled.append(frame))
            avr.receive(observed)

            # The fault is effective: the model handles Volume Up rather than
            # replying to the physically observed Give Device Power Status.
            self.assertEqual(avr.volume, 26)
            self.assertEqual(handled, [injected])
            received = [record for record in session.records if record["direction"] == "rx"]
            self.assertEqual(len(received), 1)
            record = received[0]
            self.assertEqual(record["transport"], "wire")
            self.assertEqual(record["frame"], "05:8f")
            self.assertEqual(record["opcode"], 0x8f)
            self.assertEqual(record["wire_timestamp_ns"], arrived_ns)
            self.assertEqual(record["handled_frame"], "05:44:41")

    def test_injected_identity_query_cannot_counterfeit_physical_scan_evidence(self):
        avr = self.component()
        with CecSession([avr], backend_factory=IdleBackend) as session:
            started_ns = time.monotonic_ns()
            avr.receive(Frame(0, 5, 0x83, timestamp_ns=time.monotonic_ns()))
            avr.hooks.before_rx = lambda component, frame: Frame(
                frame.source, frame.destination, 0x46,
                timestamp_ns=time.monotonic_ns() + 10**12)
            avr.receive(Frame(0, 5, 0x8f, timestamp_ns=time.monotonic_ns()))

            records = session.records
            self.assertTrue(any(record["direction"] == "tx" and record["opcode"] == 0x47
                                and record["ok"] for record in records))
            self.assertFalse(any(record["direction"] == "rx" and record["opcode"] == 0x46
                                 for record in records))
            self.assertFalse(_fresh_discovery(records, avr, 0, started_ns))

    def test_timestamp_only_injection_cannot_make_a_prescan_query_fresh(self):
        avr = self.component()
        with CecSession([avr], backend_factory=IdleBackend) as session:
            started_ns = time.monotonic_ns()
            # This timestamp is after activation, but strictly before the scan.
            # Processing occurs after the scan to model delayed RX delivery.
            stale_wire_ns = started_ns - 1
            self.assertGreaterEqual(stale_wire_ns, avr._active_since_ns)
            avr.receive(Frame(0, 5, 0x83, timestamp_ns=time.monotonic_ns()))
            avr.hooks.before_rx = lambda component, frame: Frame(
                frame.source, frame.destination, frame.opcode, frame.operands,
                timestamp_ns=time.monotonic_ns() + 10**12)
            avr.receive(Frame(0, 5, 0x46, timestamp_ns=stale_wire_ns))

            received = [record for record in session.records
                        if record["direction"] == "rx" and record["opcode"] == 0x46]
            self.assertEqual(len(received), 1)
            self.assertEqual(received[0]["wire_timestamp_ns"], stale_wire_ns)
            self.assertFalse(_fresh_discovery(session.records, avr, 0, started_ns))

    def test_rewritten_identity_replies_cannot_establish_the_expected_wire_identity(self):
        avr = self.component()
        with CecSession([avr], backend_factory=IdleBackend) as session:
            started_ns = time.monotonic_ns()
            avr.hooks.before_tx = lambda component, frame: Frame(
                frame.source, frame.destination, frame.opcode,
                b"Wrong AVR" if frame.opcode == 0x47 else b"\x20\x00\x04")
            avr.receive(Frame(0, 5, 0x46, timestamp_ns=time.monotonic_ns()))
            avr.receive(Frame(0, 5, 0x83, timestamp_ns=time.monotonic_ns()))

            replies = [record for record in session.records if record["direction"] == "tx"
                       and record["timestamp"] * 1_000_000_000 >= started_ns]
            self.assertEqual([record["frame"] for record in replies],
                             ["50:47:57:72:6f:6e:67:20:41:56:52", "5f:84:20:00:04"])
            self.assertTrue(all(record["ok"] for record in replies))
            self.assertFalse(_fresh_discovery(session.records, avr, 0, started_ns))


if __name__ == "__main__":
    unittest.main()
