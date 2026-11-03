"""Selected adapter identity needs fresh physical evidence from its owner."""

from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
import json
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecSession, DUT, Frame, Hooks, TestBed, TvComponent, TxResult
from amity_harness.physical_address import AdapterAddressEvidence


class IdentityWire:
    def __init__(self, gpio):
        self.closed = threading.Event()
        self.query = None
        self.result = None

    def open(self):
        pass

    def configure(self, components):
        pass

    def transmit(self, frame):
        result = TxResult(frame, 1, time.monotonic_ns())
        if self.query is not None and frame.opcode == 0x83:
            self.query(frame, result.timestamp_ns)
        return result if self.result is None else self.result(frame, result.timestamp_ns)

    def receive(self, timeout):
        self.closed.wait(timeout)
        return None

    def close(self):
        self.closed.set()


class AdapterAddressEvidenceTests(unittest.TestCase):
    @contextmanager
    def fixture(self, *, remote=False, back=False):
        main = DUT("amity-dut")
        owner = DUT("amity-dut-2") if remote else main
        tv = TvComponent(owner.tentacle(0).front, "Address TV")
        avr = AvrComponent(owner.tentacle(0).back, "Address AVR") if back else None
        components = (tv, avr) if back else (tv,)
        with CecSession(components, backend_factory=IdentityWire) as session:
            tv._power_timeout = .02
            bed = SimpleNamespace(session=session, _entered=True, _closed=False,
                                  duts=(main, owner) if remote else (main,))
            main._testbed = owner._testbed = bed
            boots = {main.identity.device_id: "main-boot", owner.identity.device_id: "owner-boot"}
            boots[main.identity.device_id] = "main-boot"
            scan = SimpleNamespace(dut=main, _session=session, _testbed=bed, expected=components,
                                   initiators={tv.gpio: 4, **({avr.gpio: 0} if back else {})}, owner_boots=boots,
                                   owner_duts={main.identity.device_id: main, owner.identity.device_id: owner},
                                   _boot="main-boot")
            calls = []

            def validate(components):
                calls.append(tuple(components))
                if getattr(scan, "invalid_owner", False):
                    raise AssertionError("Current owner boot changed")

            scan.validate_owners = validate
            main.last_scan = scan
            state = {"expected_boot_id": "main-boot", "discovering": False, "discovery_generation": 7,
                     "devices": [{"name": component.name, "address": component.address.logical,
                                  "physical_address": component.address.physical, "bus": component.direction}
                                 for component in components]}
            main.get_state = lambda: state
            for component in components:
                session._wire_idle_since_ns[component.gpio] = time.monotonic_ns() + 10**12
            fixture = SimpleNamespace(main=main, owner=owner, tv=tv, avr=avr, state=state, session=session,
                                      scan=scan, bed=bed, calls=calls,
                                      backend=session._backends[tv.gpio])
            fixture.backend.query = lambda frame, stamp: self.report(fixture, timestamp=stamp + 1)
            yield fixture

    def report(self, fixture, *, address=0x2000, source=4, destination=15, operands=None,
               timestamp=None, transport="wire", component=None):
        frame = Frame(source, destination, 0x84,
                      address.to_bytes(2, "big") + b"\x04" if operands is None else operands,
                      time.monotonic_ns() if timestamp is None else timestamp)
        (fixture.tv if component is None else component).receive(frame, transport=transport)
        return frame

    def test_local_and_remote_changes_require_announcement_and_query_and_retain_owner_evidence(self):
        for remote in (False, True):
            with self.subTest(remote=remote), self.fixture(remote=remote) as fixture:
                with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                    announcement = self.report(fixture)
                    self.assertIs(observation.assert_selected("2.0.0.0", changed=True), observation)
                proof = fixture.bed.control_observations[-1]
                check = proof["checks"][-1]
                self.assertEqual(proof["owner"], fixture.owner.identity.device_id)
                self.assertEqual(proof["owner_boots"], fixture.scan.owner_boots)
                self.assertEqual(check["announcement_wire_timestamp_ns"], announcement.timestamp_ns)
                self.assertLess(check["query_wire_timestamp_ns"], check["reply_wire_timestamp_ns"])
                self.assertTrue(check["passed"])
                self.assertGreaterEqual(len(fixture.calls), 3)
                self.assertFalse(fixture.tv._wire_observers)

    def test_unchanged_scanned_address_still_requires_a_fresh_query_response(self):
        with self.fixture() as fixture:
            with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                observation.assert_selected("2.0.0.0", changed=False)
            check = fixture.bed.control_observations[-1]["checks"][-1]
            self.assertNotIn("announcement_frame", check)
            self.assertEqual(check["query_frame"], "04:83")
            self.assertEqual(check["reply_frame"], "4f:84:20:00:04")

    def test_changed_address_cannot_pass_only_from_its_prompted_identity_reply(self):
        with self.assertRaisesRegex(TimeoutError, "fresh physical address report"):
            with self.fixture() as fixture:
                with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                    observation.assert_selected("2.0.0.0", changed=True)
        self.assertFalse(fixture.tv._wire_observers)

    def test_stale_virtual_wrong_owner_wrong_destination_and_late_replies_do_not_pass(self):
        for kind in ("stale", "virtual", "other_owner", "directed", "late"):
            with self.subTest(kind=kind), self.assertRaises((AssertionError, TimeoutError)):
                with self.fixture() as fixture:
                    with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                        fixture.backend.query = lambda frame, stamp: self.report(
                            fixture, timestamp=(observation._started - 1 if kind == "stale" else
                                                stamp + int(fixture.tv._power_timeout * 1e9) + 1
                                                if kind == "late" else stamp + 1),
                            transport="virtual" if kind == "virtual" else "wire",
                            source=8 if kind == "other_owner" else 4,
                            destination=0 if kind == "directed" else 15)
                        observation.assert_selected("2.0.0.0", changed=False)

    def test_old_announcement_cannot_substitute_for_query_response(self):
        with self.assertRaisesRegex(TimeoutError, "fresh physical address report"):
            with self.fixture() as fixture:
                with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                    self.report(fixture)
                    fixture.backend.query = None
                    observation.assert_selected("2.0.0.0", changed=True)

    def test_failed_virtual_rewritten_or_stale_query_cannot_pass(self):
        for kind in ("failed", "virtual", "rewritten", "stale"):
            with self.subTest(kind=kind), self.assertRaisesRegex(AssertionError, "successful fresh physical query"):
                with self.fixture() as fixture:
                    with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                        fixture.backend.result = lambda frame, stamp: TxResult(
                            Frame(frame.source, frame.destination, 0x8c) if kind == "rewritten" else frame,
                            0 if kind == "failed" else 1,
                            observation._started - 1 if kind == "stale" else stamp,
                            "virtual" if kind == "virtual" else "wire")
                        observation.assert_selected("2.0.0.0", changed=False)

    def test_wrong_address_and_type_are_rejected_even_if_responder_hook_hides_them(self):
        for operands in (b"\x30\x00\x04", b"\x20\x00\x00", b"\x20\x00"):
            with self.subTest(operands=operands), self.assertRaisesRegex(AssertionError, "unexpected physical address"):
                with self.fixture() as fixture:
                    fixture.tv.hooks = Hooks(before_rx=lambda component, frame: None)
                    with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                        self.report(fixture, operands=operands)
                        observation.assert_selected("2.0.0.0", changed=False)

    def test_unknown_wire_timestamp_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, "physical wire timestamp"):
            with self.fixture() as fixture:
                with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                    self.report(fixture, timestamp=0)
                    observation.assert_selected("2.0.0.0", changed=True)

    def test_current_owner_boot_scan_and_initiator_must_survive_change(self):
        for kind in ("current_boot", "captured_boot", "scan", "initiator"):
            with self.subTest(kind=kind), self.assertRaises(AssertionError):
                with self.fixture(remote=True) as fixture:
                    with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                        if kind == "current_boot":
                            fixture.scan.invalid_owner = True
                        elif kind == "captured_boot":
                            fixture.scan.owner_boots[fixture.owner.identity.device_id] = "other-boot"
                        elif kind == "scan":
                            fixture.main.last_scan = SimpleNamespace()
                        else:
                            fixture.scan.initiators[fixture.tv.gpio] = 8
                        observation.assert_selected("2.0.0.0", changed=False)

    def test_missing_receive_coverage_cannot_prove_no_incorrect_reports(self):
        with self.assertRaisesRegex(TimeoutError, "receive coverage did not drain"):
            with self.fixture() as fixture:
                with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                    fixture.session._wire_idle_since_ns = {}
                    observation.assert_selected("2.0.0.0", changed=False)

    def test_local_and_remote_settings_must_preserve_cache_and_main_scan_state(self):
        for remote in (False, True):
            for kind in ("generation", "invalid_discovering", "boot", "cache"):
                with self.subTest(remote=remote, kind=kind), self.assertRaisesRegex(AssertionError, "discovery|cache"):
                    with self.fixture(remote=remote) as fixture:
                        with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                            if kind == "generation":
                                fixture.state["discovery_generation"] += 1
                            elif kind == "invalid_discovering":
                                fixture.state["discovering"] = None
                            elif kind == "boot":
                                fixture.state["expected_boot_id"] = "other-boot"
                            else:
                                fixture.state["devices"] = []
                            observation.assert_selected("2.0.0.0", changed=False)

    def test_other_adapter_background_scan_is_diagnostic_for_local_and_remote_settings(self):
        for remote in (False, True):
            for before, after in ((False, False), (False, True), (True, False), (True, True)):
                with self.subTest(remote=remote, before=before, after=after), self.fixture(remote=remote) as fixture:
                    fixture.state["discovering"] = before
                    with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                        fixture.state["discovering"] = after
                        observation.assert_selected("2.0.0.0", changed=False)
                    check = fixture.bed.control_observations[-1]["checks"][-1]
                    self.assertIs(check["aggregate_discovering_before"], before)
                    self.assertIs(check["aggregate_discovering_after"], after)
                    self.assertTrue(check["passed"])
                    self.assertTrue(check["cache_unchanged"])
                    self.assertEqual(check["expected_discovery_generation"], check["observed_discovery_generation"])

    def test_background_scan_diagnostics_survive_target_wire_and_generation_failures(self):
        for remote in (False, True):
            for kind in ("target_query", "generation"):
                with self.subTest(remote=remote, kind=kind), self.fixture(remote=remote) as fixture:
                    with self.assertRaisesRegex(AssertionError, "discovery queries|rescanned"):
                        with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                            fixture.state["discovering"] = True
                            if kind == "target_query":
                                fixture.tv.hooks = Hooks(before_rx=lambda component, frame: None)
                                fixture.tv.receive(Frame(4, fixture.tv.address.logical, 0x83,
                                                         timestamp_ns=time.monotonic_ns()))
                            else:
                                fixture.state["discovery_generation"] += 1
                            observation.assert_selected("2.0.0.0", changed=False)
                    check = fixture.bed.control_observations[-1]["checks"][-1]
                    self.assertIs(check["aggregate_discovering_before"], False)
                    self.assertIs(check["aggregate_discovering_after"], True)
                    self.assertFalse(check["passed"])

    def test_unannounced_front_and_back_queries_reject_remote_scan_even_when_hook_drops_them(self):
        for side in ("front", "back"):
            with self.subTest(side=side), self.assertRaisesRegex(AssertionError, "unsolicited equipment discovery"):
                with self.fixture(remote=True, back=True) as fixture:
                    with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                        component = fixture.tv if side == "front" else fixture.avr
                        component.hooks = Hooks(before_rx=lambda component, frame: None)
                        component.receive(Frame(fixture.scan.initiators[component.gpio], component.address.logical,
                                                0x83, timestamp_ns=time.monotonic_ns()))
                        observation.assert_selected("2.0.0.0", changed=False)

    def test_testbed_report_retains_physical_proof_and_drops_unselected_private_fields(self):
        with self.fixture() as fixture:
            with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan) as observation:
                observation.assert_selected("2.0.0.0", changed=False)
            report_bed = TestBed(fixture.tv)
            report_bed.control_observations = fixture.bed.control_observations
            report_bed.control_observations[-1]["private_token"] = "PRIVATE_ADDRESS_TOKEN"
            report = report_bed.report
            self.assertTrue(report["control_observations"][-1]["checks"][-1]["passed"])
            self.assertNotIn("PRIVATE_ADDRESS_TOKEN", json.dumps(report))

    def test_an_unasserted_context_is_an_error_and_always_detaches(self):
        with self.assertRaisesRegex(RuntimeError, "was not asserted"):
            with self.fixture() as fixture:
                with AdapterAddressEvidence(fixture.main, fixture.tv, fixture.scan):
                    pass
        self.assertFalse(fixture.tv._wire_observers)


if __name__ == "__main__":
    unittest.main()
