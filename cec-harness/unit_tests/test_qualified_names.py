"""Physical negative evidence for qualified selections."""

from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, Hooks, TestBed, TvComponent
from amity_harness.cec import Frame
from amity_harness.qualified_names import observe_no_control
from unit_tests.virtual_time import use_virtual_time


class NegativeControlEvidenceTests(unittest.TestCase):
    def setUp(self):
        # Wire stamps and the observation window and drain tail share one virtual clock.
        self.clock = use_virtual_time(self)
        self.dut = DUT("amity-dut")
        self.component = TvComponent(self.dut.tentacle(0).front, "Same TV",
                                     hooks=Hooks(before_rx=lambda component, frame: None))
        self.component.address = SimpleNamespace(logical=0)
        self.records = []
        self.session = SimpleNamespace(_lock=threading.RLock(), _stop=threading.Event(),
                                       _wire_idle_since_ns={self.component.gpio: 10**30},
                                       assert_healthy=lambda: None,
                                       _record=lambda *args, **kw: self.records.append((args, kw)))
        self.bed = SimpleNamespace(session=self.session, _entered=True, _closed=False)
        self.dut._testbed = self.bed
        self.component._session = self.session
        self.scan = SimpleNamespace(dut=self.dut, expected=(self.component,), _session=self.session,
                                    _testbed=self.bed, initiators={self.component.gpio: 4},
                                    validate_owners=lambda components: None)
        self.dut.last_scan = self.scan
        self.component._wire_context = lambda since: (self.session, since, 4, None)

    def receive(self, opcode, *, source=4, transport="wire", timestamp=None):
        self.component.receive(Frame(source, 0, opcode,
                                     timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp),
                               transport=transport)

    def test_control_on_inactive_wire_fails_even_when_responder_hook_drops_it(self):
        for opcode in (0x04, 0x0d, 0x36, 0x44, 0x45, 0x80, 0x81, 0x82, 0x86, 0x9d):
            with self.subTest(opcode=opcode):
                with self.assertRaisesRegex(AssertionError, "inactive owner's wire"):
                    with observe_no_control(self.scan, self.component, duration=1) as observation:
                        self.receive(opcode)
                        observation.assert_clear()
                self.assertFalse(self.component._wire_observers)

    def test_virtual_stale_and_peer_control_cannot_become_dut_wire_evidence(self):
        with observe_no_control(self.scan, self.component, duration=1) as observation:
            self.receive(0x44, transport="virtual")
            self.receive(0x44, timestamp=1)
            self.receive(0x44, source=8)
            self.receive(0x83)
            observation.assert_clear()
        self.assertEqual(len(self.bed.control_observations), 1)
        proof = self.bed.control_observations[0]
        self.assertEqual(proof["kind"], "no_owner_control")
        self.assertEqual(proof["wires"], [self.component.gpio])
        self.assertGreaterEqual(proof["idle_receive_since_ns"][str(self.component.gpio)],
                                proof["observation_end_monotonic_ns"])

    def test_missing_wire_clock_cannot_pass(self):
        with self.assertRaisesRegex(AssertionError, "without a wire timestamp"):
            with observe_no_control(self.scan, self.component, duration=1) as observation:
                self.receive(0x83, timestamp=0)
                observation.assert_clear()

    def test_missing_receive_coverage_and_changed_owner_proof_fail(self):
        self.session._wire_idle_since_ns.clear()
        with self.assertRaisesRegex(TimeoutError, "did not drain"):
            with observe_no_control(self.scan, self.component, duration=1) as observation:
                observation.assert_clear()
        self.session._wire_idle_since_ns[self.component.gpio] = 10**30
        with self.assertRaisesRegex(AssertionError, "different active TestBed"):
            with observe_no_control(self.scan, self.component, duration=1) as observation:
                self.dut.last_scan = None
                observation.assert_clear()

    def test_unasserted_observation_fails_but_does_not_hide_an_existing_failure(self):
        with self.assertRaisesRegex(RuntimeError, "not asserted"):
            with observe_no_control(self.scan, self.component, duration=1):
                pass
        with self.assertRaisesRegex(ValueError, "original failure"):
            with observe_no_control(self.scan, self.component, duration=1):
                raise ValueError("original failure")
        self.assertFalse(self.component._wire_observers)

    def test_report_retains_bounded_negative_proof_without_arbitrary_fields(self):
        dut = DUT("amity-dut")
        bed = TestBed(dut)
        record = {"kind": "no_owner_control", "owner_ids": [dut.identity.device_id],
                  "wires": [23], "since_monotonic_ns": 100, "observation_end_monotonic_ns": 200,
                  "idle_receive_since_ns": {"23": 201}, "observed_frame_count": 4,
                  "password": "must not appear"}
        bed.control_observations = [record]
        proof, = bed.report["control_observations"]
        self.assertNotIn("password", proof)
        self.assertEqual(proof["idle_receive_since_ns"], {"23": 201})
        record["owner_ids"].append("later mutation")
        self.assertEqual(proof["owner_ids"], [dut.identity.device_id])


if __name__ == "__main__":
    unittest.main()
