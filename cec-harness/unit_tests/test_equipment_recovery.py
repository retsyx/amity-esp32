"""Missing equipment retains saved intent and needs fresh physical return proof."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, Frame, TestBed
from amity_harness.recovery import LookupScans, _fresh_return, assert_returned, assert_switch_commands
from test_activity import Fixture
import test_keys as key_fixture


class SavedEquipmentTests(unittest.TestCase):
    def activity(self):
        fixture = Fixture()
        activity = fixture.dut.create_activity("Missing equipment", display=fixture.tv,
                                               source=fixture.player, audio=fixture.avr)
        return fixture, activity

    def test_saved_activity_can_start_and_remain_active_without_live_source_observation(self):
        fixture, activity = self.activity()
        original = fixture.state

        def absent():
            state = original()
            state["devices"] = [row for row in state["devices"]
                                if row["name"] != fixture.selectors[fixture.player.name]]
            return state

        fixture.dut.get_state = absent
        fixture.source.system["observations"] = fixture.source.system["observations"][:2]
        activity.start()
        activity.assert_active()
        self.assertEqual(fixture.rows[-1], activity._created_row)
        self.assertTrue(any(path == "/api/activity-editor/activity" for path, _ in fixture.calls))

    def test_changed_saved_qualified_owner_adapter_side_name_or_group_is_rejected(self):
        for field, value in (("octopus_id", "e" * 32), ("tentacle_id", 1),
                             ("side", "front"), ("osd_name", "Other"), ("consortium_id", "e" * 32)):
            with self.subTest(field=field):
                fixture, activity = self.activity()
                identity = fixture.selectors[fixture.player.name].removeprefix("component:")
                record = next(record for record in fixture.full["components"] if record["component_id"] == identity)
                record["qualified_names"][0][field] = value
                if field == "consortium_id":
                    fixture.full["consortium_id"] = value
                before = len(fixture.calls)
                with self.assertRaisesRegex(AssertionError, "equipment changed"):
                    activity.start()
                self.assertFalse(any(path == "/api/activity-editor/activity"
                                     for path, _ in fixture.calls[before:]))


class EquipmentRecoveryTests(unittest.TestCase):
    def fixture(self):
        fixture = key_fixture.KeyTests().fixture()
        scan = fixture.dut.last_scan
        scan._testbed, scan._group = fixture.bed, fixture.source.group
        scan.owner_boots = {fixture.dut.identity.device_id: fixture.source.boot}
        scan.owner_duts = {fixture.dut.identity.device_id: fixture.dut}
        fixture.session._wire_idle_since_ns = {component.gpio: time.monotonic_ns() + 10**12
                                               for component in fixture.source.components}
        fixture.generation = scan.status["discovery_generation"]
        original = fixture.dut.get_state
        fixture.dut.get_state = lambda: {**original(), "discovery_generation": fixture.generation}
        return fixture, scan

    def record(self, fixture, component, direction, frame, *, transport="wire", **extra):
        fixture.session.records.append({"timestamp": frame.timestamp_ns / 1e9, "direction": direction,
                                        "component": component.name, "gpio": component.gpio,
                                        "transport": transport, "source": frame.source,
                                        "destination": frame.destination, "opcode": frame.opcode,
                                        "frame": frame.raw.hex(":"), "wire_timestamp_ns": frame.timestamp_ns,
                                        **extra})

    def identity(self, fixture, observer, component):
        initiator = fixture.dut.last_scan.initiators[component.gpio]
        for query, reply, operands in ((0x83, 0x84, component.address.physical.to_bytes(2, "big")
                                       + bytes([component.device_type])),
                                      (0x46, 0x47, component.name.encode("ascii"))):
            stamp = time.monotonic_ns()
            request = Frame(initiator, component.address.logical, query, timestamp_ns=stamp)
            observer._observe(component, request, "wire")
            self.record(fixture, component, "rx", request)
            answer = Frame(component.address.logical, 15 if reply == 0x84 else initiator,
                           reply, operands, timestamp_ns=stamp + 1)
            self.record(fixture, component, "tx", answer, ok=True)

    def announcement(self, fixture, component, *, timestamp=None, **extra):
        stamp = time.monotonic_ns() if timestamp is None else timestamp
        frame = Frame(component.address.logical, 15, 0x84,
                      component.address.physical.to_bytes(2, "big") + bytes([component.device_type]),
                      timestamp_ns=stamp)
        self.record(fixture, component, "tx", frame, ok=True, **extra)

    def query(self, fixture, observer, component, opcode=0x83, *, timestamp=None):
        stamp = time.monotonic_ns() if timestamp is None else timestamp
        frame = Frame(fixture.dut.last_scan.initiators[component.gpio], component.address.logical,
                      opcode, timestamp_ns=stamp)
        observer._observe(component, frame, "wire")
        self.record(fixture, component, "rx", frame)

    def test_cooldown_checks_reject_an_actual_unsolicited_identity_query(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            observer.assert_no_scan()
            self.identity(fixture, observer, fixture.avr)
            with self.assertRaisesRegex(AssertionError, "unsolicited"):
                observer.assert_no_scan()
        self.assertFalse(fixture.tv._wire_observers or fixture.avr._wire_observers)

    def test_scoped_witness_assertion_retains_unrelated_aggregate_discovery(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            original = fixture.dut.get_state
            fixture.generation += 1
            fixture.dut.get_state = lambda: {**original(), "discovering": True}
            observer.assert_no_scan_on_witnesses()
            check = observer._report["checks"][-1]
            self.assertEqual(check["kind"], "no_scan_on_witnesses")
            self.assertTrue(check["passed"])
            self.assertTrue(check["discovering"])
            self.assertNotEqual(check["expected_generation"], check["observed_generation"])
            self.assertEqual(check["frames"], [])
            self.assertEqual(observer._report["wires"], [
                {"owner": component.dut.identity.device_id, "tentacle": component.tentacle,
                 "side": component.direction, "gpio": component.gpio}
                for component in (fixture.tv, fixture.avr)])
            with self.assertRaises(AssertionError):
                observer.assert_no_scan()
        self.assertFalse(fixture.calls)

    def test_scoped_witness_assertion_still_rejects_real_queries_and_invalid_clock(self):
        for opcode, timestamp in ((0x83, None), (0x46, None), (0x83, 0)):
            with self.subTest(opcode=opcode, timestamp=timestamp):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    original = fixture.dut.get_state
                    fixture.dut.get_state = lambda: {**original(), "discovering": True}
                    self.query(fixture, observer, fixture.avr, opcode, timestamp=timestamp)
                    with self.assertRaisesRegex(AssertionError, "unsolicited"):
                        observer.assert_no_scan_on_witnesses()
                    check = observer._report["checks"][-1]
                    self.assertFalse(check["passed"])
                    self.assertEqual(check["invalid_clock"], timestamp == 0)
                    if timestamp != 0:
                        self.assertEqual(check["unexpected_queries"][0]["query_frame"], f"05:{opcode:02x}")

    def test_scoped_witness_assertion_requires_original_boot_and_current_owner_proof(self):
        for changed in ("status_boot", "owner_boot"):
            with self.subTest(changed=changed):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    if changed == "status_boot":
                        original = fixture.dut.get_state
                        fixture.dut.get_state = lambda: {**original(), "expected_boot_id": "e" * 32}
                    else:
                        fixture.source.system["boot_id"] = "e" * 32
                    with self.assertRaises(AssertionError):
                        observer.assert_no_scan_on_witnesses()

    def test_scoped_assertions_require_unbroken_registered_observer_lifetime(self):
        for inactive in ("unentered", "closed", "unregistered"):
            for assertion in ("no_scan", "passive_return"):
                with self.subTest(inactive=inactive, assertion=assertion):
                    fixture, scan = self.fixture()
                    scan.expected = (fixture.tv, fixture.avr)
                    observer = LookupScans(scan, fixture.tv, fixture.avr)
                    if inactive != "unentered":
                        observer.__enter__()
                    since = time.monotonic_ns()
                    self.return_records(fixture, fixture.player, since)
                    original = fixture.dut.get_state
                    fixture.state = fixture.dut.get_state = lambda: {**original(), "discovering": True}
                    if inactive == "closed":
                        observer.__exit__(None, None, None)
                    elif inactive == "unregistered":
                        fixture.avr._wire_observers.remove(observer._observe)
                    try:
                        with self.assertRaisesRegex(AssertionError, "active registered wire observation"):
                            if assertion == "no_scan":
                                observer.assert_no_scan_on_witnesses()
                            else:
                                observer.assert_passive_return(fixture.player, since=since)
                        self.assertNotIn(fixture.player, scan.expected)
                    finally:
                        if inactive == "unregistered":
                            observer.__exit__(None, None, None)

    def test_existing_avr_reannouncement_permits_only_its_fresh_targeted_metadata(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            self.announcement(fixture, fixture.avr)
            self.identity(fixture, observer, fixture.avr)
            observer.assert_no_scan()
            check = observer._report["checks"][-1]
            self.assertTrue(check["passed"])
            self.assertEqual(check["unexpected_queries"], [])
            self.assertEqual(check["expected_generation"], check["observed_generation"])
            self.assertEqual(len(check["metadata_queries"]), 2)
            for query in check["metadata_queries"]:
                self.assertEqual(query["component"], fixture.avr.name)
                self.assertEqual(query["owner"], fixture.dut.identity.device_id)
                self.assertEqual(query["gpio"], fixture.avr.gpio)
                self.assertLess(query["announcement_wire_timestamp_ns"], query["query_wire_timestamp_ns"])
        self.assertFalse(fixture.calls)

    def test_unproven_reannouncement_cannot_exempt_an_identity_query(self):
        for wrong in ("virtual", "stale", "expired", "wire", "component", "owner",
                      "rewritten", "physical_address", "failed", "after_query"):
            with self.subTest(wrong=wrong):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    query_at = time.monotonic_ns() + 10_000
                    self.announcement(fixture, fixture.avr, timestamp=query_at - 1)
                    row = fixture.session.records[-1]
                    if wrong == "virtual":
                        row["transport"] = "virtual"
                    elif wrong == "stale":
                        row["wire_timestamp_ns"] = observer._started - 1
                    elif wrong == "expired":
                        row["wire_timestamp_ns"] = observer._started
                        query_at = observer._started + int(scan.dut._runtime.cec_timeout * 1e9) + 1
                    elif wrong == "wire":
                        row["gpio"] = fixture.tv.gpio
                    elif wrong == "component":
                        row["component"] = fixture.tv.name
                    elif wrong == "owner":
                        row["source"] = fixture.player.address.logical
                    elif wrong == "rewritten":
                        row["handled_frame"] = "5f:84:20:00:05"
                    elif wrong == "physical_address":
                        row["frame"] = "5f:84:20:00:05"
                    elif wrong == "failed":
                        row["ok"] = False
                    elif wrong == "after_query":
                        row["wire_timestamp_ns"] = query_at + 1
                    self.query(fixture, observer, fixture.avr, timestamp=query_at)
                    with self.assertRaisesRegex(AssertionError, "unsolicited"):
                        observer.assert_no_scan()
                    check = observer._report["checks"][-1]
                    self.assertFalse(check["passed"])
                    self.assertEqual(check["metadata_queries"], [])
                    self.assertEqual(check["unexpected_queries"][0]["query_frame"], "05:83")

    def test_malformed_metadata_query_is_not_exempted_by_an_announcement(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            self.announcement(fixture, fixture.avr)
            frame = Frame(scan.initiators[fixture.avr.gpio], fixture.avr.address.logical,
                          0x46, b"\xff", timestamp_ns=time.monotonic_ns())
            observer._observe(fixture.avr, frame, "wire")
            with self.assertRaisesRegex(AssertionError, "unsolicited"):
                observer.assert_no_scan()

    def test_cooldown_early_failure_retains_the_offending_physical_query(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            self.query(fixture, observer, fixture.avr)
            with self.assertRaisesRegex(AssertionError, "deferred a scan"):
                observer.wait_for_cooldown()
            check = observer._report["checks"][-1]
            self.assertEqual(check["kind"], "cooldown")
            self.assertFalse(check["passed"])
            self.assertEqual(check["unexpected_queries"][0]["query_frame"], "05:83")

    def test_announcement_does_not_exempt_another_wire_or_a_real_scan(self):
        for wrong in ("wire", "generation", "discovering"):
            with self.subTest(wrong=wrong):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    self.announcement(fixture, fixture.avr)
                    self.query(fixture, observer, fixture.tv if wrong == "wire" else fixture.avr)
                    if wrong == "generation":
                        fixture.generation += 1
                    elif wrong == "discovering":
                        original = fixture.dut.get_state
                        fixture.dut.get_state = lambda: {**original(), "discovering": True}
                    with self.assertRaises(AssertionError):
                        observer.assert_no_scan()
                    self.assertFalse(observer._report["checks"][-1]["passed"])

    def test_metadata_correlations_and_rejected_queries_are_retained_in_report(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            self.announcement(fixture, fixture.avr)
            self.query(fixture, observer, fixture.avr, opcode=0x46)
            self.query(fixture, observer, fixture.tv)
            with self.assertRaisesRegex(AssertionError, "unsolicited"):
                observer.assert_no_scan()
            bed = TestBed(fixture.dut)
            bed.control_observations = [observer._report]
            report = bed.report["control_observations"][0]
            check = report["checks"][-1]
            self.assertEqual(len(check["metadata_queries"]), 1)
            self.assertEqual(check["unexpected_queries"][0]["component"], fixture.tv.name)
            self.assertEqual(check["expected_boot"], scan._boot)
            self.assertEqual(check["observed_boot"], scan._boot)
            self.assertFalse(check["passed"])
            observer._report["checks"].clear()
            self.assertEqual(len(report["checks"]), 1)

    def test_expiry_alone_is_observed_without_issuing_an_operation(self):
        fixture, scan = self.fixture()
        with patch("amity_harness.recovery.LOOKUP_COOLDOWN_SECONDS", 0):
            with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                observer.wait_for_cooldown()
        self.assertFalse(fixture.calls)

    def test_back_scan_requires_fresh_requests_replies_and_preserved_front_cache(self):
        for wrong in (None, "opposite", "replies", "front_cache", "generation"):
            with self.subTest(wrong=wrong):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    self.identity(fixture, observer, fixture.avr)
                    if wrong != "generation":
                        fixture.generation += 1
                    if wrong == "opposite":
                        self.identity(fixture, observer, fixture.tv)
                    elif wrong == "replies":
                        fixture.session.records = [row for row in fixture.session.records
                                                   if row["wire_timestamp_ns"] < observer._started
                                                   or row["direction"] != "tx"]
                    elif wrong == "front_cache":
                        original = fixture.dut.get_state
                        fixture.dut.get_state = lambda: {**original(), "devices": original()["devices"][1:]}
                    if wrong is None:
                        observer.assert_scan_on(fixture.avr.endpoint)
                    else:
                        with self.assertRaises(AssertionError):
                            observer.assert_scan_on(fixture.avr.endpoint)

    def test_back_scan_requires_exactly_one_enumeration_of_the_scanned_side(self):
        for extra in (None, 0x83, 0x46):
            with self.subTest(extra=extra):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    self.identity(fixture, observer, fixture.avr)
                    fixture.generation += 1
                    if extra is None:
                        observer.assert_scan_on(fixture.avr.endpoint)
                    else:
                        self.query(fixture, observer, fixture.avr, extra)
                        with self.assertRaisesRegex(AssertionError, "exactly once"):
                            observer.assert_scan_on(fixture.avr.endpoint)

    def unwitnessed_identity(self, fixture, component):
        # Only witnesses register the observer; the recipient has records only.
        initiator = fixture.dut.last_scan.initiators[component.gpio]
        for opcode in (0x83, 0x46):
            self.record(fixture, component, "rx", Frame(initiator, component.address.logical, opcode,
                                                        timestamp_ns=time.monotonic_ns()))

    def test_lookup_of_requires_one_enumeration_of_the_recipient_and_none_on_witnesses(self):
        for wrong in (None, "twice", "missing", "witness"):
            with self.subTest(wrong=wrong):
                fixture, scan = self.fixture()
                with LookupScans(scan, fixture.tv) as observer:
                    if wrong != "missing":
                        self.unwitnessed_identity(fixture, fixture.player)
                    if wrong == "twice":
                        self.unwitnessed_identity(fixture, fixture.player)
                    elif wrong == "witness":
                        self.query(fixture, observer, fixture.tv)
                    if wrong is None:
                        observer.assert_lookup_of(fixture.player)
                        check = observer._report["checks"][-1]
                        self.assertEqual((check["kind"], check["passed"], len(check["frames"])),
                                         ("lookup_of", True, 2))
                    else:
                        with self.assertRaises(AssertionError):
                            observer.assert_lookup_of(fixture.player)

    def test_lookup_of_rejects_a_witness_on_the_recipients_side(self):
        fixture, scan = self.fixture()
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            with self.assertRaisesRegex(ValueError, "other CEC sides"):
                observer.assert_lookup_of(fixture.player)

    def return_records(self, fixture, component, since):
        initiator = fixture.dut.last_scan.initiators[component.gpio]
        self.record(fixture, component, "tx", Frame(component.address.logical, 15, 0x84,
                    component.address.physical.to_bytes(2, "big") + bytes([component.device_type]),
                    timestamp_ns=since + 1), ok=True)
        self.record(fixture, component, "rx", Frame(initiator, component.address.logical, 0x46,
                    timestamp_ns=since + 2))
        self.record(fixture, component, "tx", Frame(component.address.logical, initiator, 0x47,
                    component.name.encode("ascii"), timestamp_ns=since + 3), ok=True)

    def test_passive_return_extends_proof_only_after_new_wire_identity_and_typed_ui_match(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        since = time.monotonic_ns()
        self.return_records(fixture, fixture.player, since)
        assert_returned(scan, fixture.player, since=since)
        self.assertIn(fixture.player, scan.expected)
        proof = fixture.bed.control_observations[-1]
        self.assertLess(proof["announcement_wire_timestamp_ns"], proof["query_wire_timestamp_ns"])
        self.assertLess(proof["query_wire_timestamp_ns"], proof["reply_wire_timestamp_ns"])
        self.assertFalse(fixture.calls)

    def test_timely_return_frames_survive_later_owner_and_ui_confirmation(self):
        for scoped in (False, True):
            with self.subTest(scoped=scoped):
                fixture, scan = self.fixture()
                scan.expected = (fixture.tv, fixture.avr)
                with LookupScans(scan, fixture.tv, fixture.avr) as observer:
                    since = time.monotonic_ns()
                    self.return_records(fixture, fixture.player, since)
                    clock = {"now": since}
                    deadline = since + 1 + int(fixture.dut._runtime.cec_timeout * 1e9)
                    validate = scan.validate_owners

                    def slow_owner(*args, **kwargs):
                        result = validate(*args, **kwargs)
                        clock["now"] = max(clock["now"], deadline + 1_000_000_000)
                        return result

                    state = fixture.dut.get_state

                    def late_ui():
                        clock["now"] = deadline + 3_000_000_000
                        return {**state(), "discovering": scoped}

                    scan.validate_owners = slow_owner
                    fixture.dut.get_state = late_ui
                    fixture.dut.wait_for = Mock(wraps=DUT.wait_for.__get__(fixture.dut))
                    with patch("amity_harness.recovery.time.monotonic_ns", side_effect=lambda: clock["now"]):
                        if scoped:
                            observer.assert_passive_return(fixture.player, since=since)
                        else:
                            assert_returned(scan, fixture.player, since=since)
                    self.assertIn(fixture.player, scan.expected)
                    self.assertEqual(fixture.dut.wait_for.call_args.kwargs["timeout"], fixture.dut._runtime.timeout)
                    proof = fixture.bed.control_observations[-1]
                    self.assertEqual(proof["deadline_monotonic_ns"], deadline)
                    self.assertLessEqual(proof["reply_wire_timestamp_ns"], deadline)

    def test_expired_window_without_timely_return_fails_before_waiting_for_ui(self):
        for physical in ("late", "missing"):
            with self.subTest(physical=physical):
                fixture, scan = self.fixture()
                scan.expected = (fixture.tv, fixture.avr)
                since = time.monotonic_ns()
                deadline = since + 1 + int(fixture.dut._runtime.cec_timeout * 1e9)
                if physical == "late":
                    self.return_records(fixture, fixture.player, deadline + 1)
                    fixture.session.records[-3]["wire_timestamp_ns"] = since + 1
                fixture.dut.wait_for = Mock(side_effect=AssertionError("An expired wire window must not poll UI"))
                with patch("amity_harness.recovery.time.monotonic_ns", return_value=deadline + 1_000_000_000):
                    with self.assertRaisesRegex(TimeoutError, "physical CEC deadline"):
                        assert_returned(scan, fixture.player, since=since)
                fixture.dut.wait_for.assert_not_called()
                self.assertNotIn(fixture.player, scan.expected)

    def test_address_claim_setup_does_not_consume_the_physical_return_window(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        since = time.monotonic_ns()
        self.return_records(fixture, fixture.player, since)
        announced = since + 2_000_000_000
        # Linux announced its shared PA while reclaiming addresses. The DUT
        # queried promptly, before the harness corrected the player's PA.
        fixture.session.records[-3]["wire_timestamp_ns"] = announced
        fixture.session.records[-2]["wire_timestamp_ns"] = announced - 500_000_000
        fixture.session.records[-1]["wire_timestamp_ns"] = announced + 800_000_000
        with patch("amity_harness.recovery.time.monotonic_ns", return_value=announced + 900_000_000):
            assert_returned(scan, fixture.player, since=since)
        proof = fixture.bed.control_observations[-1]
        self.assertEqual(proof["since_monotonic_ns"], since)
        self.assertEqual(proof["announcement_wire_timestamp_ns"], announced)
        self.assertEqual(proof["deadline_monotonic_ns"], announced + 1_000_000_000)
        self.assertIn(fixture.player, scan.expected)

    def test_reannouncement_cannot_extend_an_incomplete_first_return_window(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        since = time.monotonic_ns()
        self.announcement(fixture, fixture.player, timestamp=since + 1)
        deadline = since + 1 + int(fixture.dut._runtime.cec_timeout * 1e9)
        # A second correct announcement has a complete exchange, but all of
        # it is too late for the first physical identity announcement.
        self.return_records(fixture, fixture.player, deadline + 1)
        fixture.dut.wait_for = Mock(side_effect=AssertionError("An expired wire window must not poll UI"))
        with patch("amity_harness.recovery.time.monotonic_ns", return_value=deadline + 100):
            with self.assertRaisesRegex(TimeoutError, "physical CEC deadline"):
                assert_returned(scan, fixture.player, since=since)
        fixture.dut.wait_for.assert_not_called()
        self.assertNotIn(fixture.player, scan.expected)

    def test_scoped_passive_return_requires_genuine_identity_despite_other_adapter_discovery(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            original = fixture.dut.get_state
            fixture.state = fixture.dut.get_state = lambda: {**original(), "discovering": True}
            since = time.monotonic_ns()
            self.return_records(fixture, fixture.player, since)
            with self.assertRaisesRegex(AssertionError, "full scan"):
                assert_returned(scan, fixture.player, since=since)
            self.assertNotIn(fixture.player, scan.expected)
            observer.assert_passive_return(fixture.player, since=since)
            self.assertIn(fixture.player, scan.expected)
            check = observer._report["checks"][-1]
            self.assertTrue(check["discovering"])
            self.assertTrue(check["passed"])
            self.assertEqual(check["kind"], "no_scan_on_witnesses")
            proof = fixture.bed.control_observations[-1]
            self.assertEqual(proof["component"], fixture.player.name)
            self.assertLess(proof["query_wire_timestamp_ns"], proof["reply_wire_timestamp_ns"])

    def test_scoped_passive_return_rejects_unannounced_query_before_admitting_peer(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        with LookupScans(scan, fixture.tv, fixture.avr) as observer:
            original = fixture.dut.get_state
            fixture.state = fixture.dut.get_state = lambda: {**original(), "discovering": True}
            since = time.monotonic_ns()
            self.return_records(fixture, fixture.player, since)
            self.query(fixture, observer, fixture.avr)
            with self.assertRaisesRegex(AssertionError, "unsolicited"):
                observer.assert_passive_return(fixture.player, since=since)
            self.assertNotIn(fixture.player, scan.expected)
            self.assertFalse(observer._report["checks"][-1]["passed"])

    def test_scoped_passive_return_requires_a_witness_on_its_exact_wire(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        with LookupScans(scan, fixture.tv) as observer:
            since = time.monotonic_ns()
            self.return_records(fixture, fixture.player, since)
            with self.assertRaisesRegex(AssertionError, "same owner and CEC side"):
                observer.assert_passive_return(fixture.player, since=since)
            self.assertNotIn(fixture.player, scan.expected)

    def test_virtual_stale_wrong_recipient_and_modified_return_frames_cannot_pass(self):
        for wrong in ("virtual", "stale", "recipient", "rewritten", "failed", "order"):
            with self.subTest(wrong=wrong):
                fixture, scan = self.fixture()
                since = time.monotonic_ns()
                self.return_records(fixture, fixture.player, since)
                records = deepcopy(fixture.session.records[-3:])
                if wrong == "virtual":
                    records[0]["transport"] = "virtual"
                elif wrong == "stale":
                    records[0]["wire_timestamp_ns"] = since - 1
                elif wrong == "recipient":
                    records[1]["frame"] = "05:46"
                elif wrong == "rewritten":
                    records[1]["handled_frame"] = "04:83"
                elif wrong == "failed":
                    records[2]["ok"] = False
                else:
                    records[2]["wire_timestamp_ns"] = since + 1
                self.assertIsNone(_fresh_return(records, fixture.player, 0, since, since + 100))

    def test_fresh_metadata_can_precede_the_corrected_component_pa_announcement(self):
        fixture, scan = self.fixture()
        scan.expected = (fixture.tv, fixture.avr)
        since = time.monotonic_ns()
        self.return_records(fixture, fixture.player, since)
        fixture.session.records[-3]["wire_timestamp_ns"] = since + 4
        assert_returned(scan, fixture.player, since=since)
        proof = fixture.bed.control_observations[-1]
        self.assertLess(proof["reply_wire_timestamp_ns"], proof["announcement_wire_timestamp_ns"])

    def test_fallback_must_run_once_and_stop_when_the_source_returns(self):
        for count in (0, 1, 2):
            with self.subTest(count=count):
                fixture, scan = self.fixture()
                boundary = SimpleNamespace(dut=fixture.dut, session=fixture.session,
                                           started_ns=time.monotonic_ns(), initiators=scan.initiators)
                for _ in range(count):
                    self.record(fixture, fixture.avr, "rx", Frame(0, 5, 0x44, b"\x69\x04",
                                                                timestamp_ns=time.monotonic_ns()))
                if count == 1:
                    assert_switch_commands(fixture.avr, 4, since=boundary, count=1)
                    with self.assertRaises(AssertionError):
                        assert_switch_commands(fixture.avr, 4, since=boundary, count=0)
                else:
                    with self.assertRaises(AssertionError):
                        assert_switch_commands(fixture.avr, 4, since=boundary, count=1)


if __name__ == "__main__":
    unittest.main()
