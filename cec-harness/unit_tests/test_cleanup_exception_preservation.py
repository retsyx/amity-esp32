"""Context cleanup preserves the original failure and still retries/fails safely."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from copy import deepcopy
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import TestBed
from amity_harness.activity import ActivityCleanupError
from amity_harness.dut import AmityHTTPError
from amity_harness.membership import MembershipCleanupError
from amity_harness.membership import AddedAmity
from amity_harness.testbed import TestBedCleanupError
from test_activity import Fixture as ActivityFixture
from test_membership import Fixture as MembershipFixture, UnitSession


class CleanupExceptionTests(unittest.TestCase):
    @contextmanager
    def activities(self):
        fixture = ActivityFixture()
        fixture.add_member()
        old_bed = fixture.bed
        session = UnitSession(fixture.source.components)
        session.records = fixture.session.records
        fixture.session = session
        for component in fixture.source.components:
            component._session = session
        fixture.source.result._session = session
        fixture.dut._testbed = fixture.member._testbed = None
        fixture.dut.authenticate = lambda: setattr(fixture.dut, "_token", "PRIVATE_UNIT_TOKEN")
        fixture.dut.logout = lambda: setattr(fixture.dut, "_token", None)
        bed = TestBed(fixture.dut, fixture.member, members=(fixture.member,), session_factory=lambda components: session)
        fixture.bed = bed
        fixture.source.result._testbed = bed
        bed._member_proofs = old_bed._member_proofs
        for proof in bed._member_proofs.values():
            proof.session = session
        with bed:
            yield fixture, bed

    def test_body_failure_survives_activity_standby_fence_and_successful_bed_retry(self):
        original = TimeoutError("missing directed source-theft Standby")
        bed = None
        with self.assertRaises(TimeoutError) as raised:
            with self.activities() as (fixture, bed):
                with ExitStack() as contexts:
                    selected = contexts.enter_context(fixture.activity("Selected"))
                    contexts.enter_context(fixture.activity("Unused", display=fixture.member_tv,
                        audio=fixture.member_avr, source=fixture.member_player))
                    selected.start()
                    raise original
        self.assertIs(raised.exception, original)
        self.assertEqual(bed.report["error"], "TimeoutError")
        self.assertEqual(bed.report["cleanup_errors"], [])
        cleanups = [row for row in bed.report["activities"] if row["phase"] == "cleanup"]
        self.assertFalse(any(row.get("error_type") for row in cleanups))
        self.assertTrue(any(row["name"] == "Unused" and row.get("removed_activity") is True for row in cleanups))
        self.assertEqual(len(fixture.rows), 1)

    def test_unused_activity_waits_for_selected_fixture_context_before_row_cleanup(self):
        with self.activities() as (fixture, bed):
            with ExitStack() as contexts:
                selected = contexts.enter_context(fixture.activity("Selected"))
                unused = contexts.enter_context(fixture.activity("Unused", display=fixture.member_tv,
                    audio=fixture.member_avr, source=fixture.member_player))
                selected.start()
            self.assertFalse(unused._closed)
            self.assertTrue(selected._closed)
            # Registered after the deferred activity: plain LIFO would restore
            # settings first. Resource phases must finish the activity rows.
            bed.add_cleanup(lambda: self.assertTrue(unused._closed))
        self.assertNotIn("error", bed.report)
        self.assertEqual(bed.report["cleanup_errors"], [])
        self.assertEqual(len(fixture.rows), 1)

    def test_deferred_shared_member_equipment_is_pruned_in_both_row_orders(self):
        for reverse_creation in (False, True):
            with self.subTest(reverse_creation=reverse_creation):
                with self.activities() as (fixture, bed):
                    fixture.full.update(consortium_id=fixture.source.group,
                                        octopuses=[{"octopus_id": fixture.dut.identity.device_id},
                                                   {"octopus_id": fixture.member.identity.device_id}])
                    baseline = deepcopy(fixture.full)
                    options = {"display": fixture.member_tv, "audio": fixture.member_avr}
                    if reverse_creation:
                        unused = fixture.activity("Inner", source=fixture.player, **options)
                        selected = fixture.activity("Outer", source=fixture.member_player, **options)
                    else:
                        selected = fixture.activity("Outer", source=fixture.member_player, **options)
                        unused = fixture.activity("Inner", source=fixture.player, **options)
                    with selected:
                        with unused:
                            selected.start()
                    self.assertFalse(unused._closed)
                    self.assertTrue(selected._closed)
                    # Use the same saved-baseline checker that blocks member
                    # removal when any fixture equipment is accidentally left.
                    member = object.__new__(AddedAmity)
                    member.main, member.candidate = fixture.dut, fixture.member
                    member.device_id, member.group = fixture.member.identity.device_id, fixture.source.group
                    member._baseline = baseline
                    member._request = lambda path, data=None, **kwargs: fixture.dut.request(path, data)
                    member._removed_owners = lambda: set()
                self.assertEqual(fixture.full["components"], baseline["components"])
                self.assertEqual(member._config(deadline=time.monotonic() + 1)["components"], baseline["components"])
                traces = [row for row in bed.report["activities"] if row["phase"] == "cleanup"]
                self.assertEqual(len(traces), 2)
                self.assertTrue(all(row["removed_activity"] and not row["retained_equipment"] for row in traces))

    def test_sibling_row_removed_on_retry_rechecks_closed_activity_equipment(self):
        with self.activities() as (fixture, bed):
            fixture.full.update(consortium_id=fixture.source.group,
                                octopuses=[{"octopus_id": fixture.dut.identity.device_id},
                                           {"octopus_id": fixture.member.identity.device_id}])
            baseline = deepcopy(fixture.full)
            options = {"display": fixture.member_tv, "audio": fixture.member_avr}
            selected = fixture.activity("Outer", source=fixture.member_player, **options)
            unused = fixture.activity("Inner", source=fixture.player, **options)
            with selected:
                with unused:
                    selected.start()
            self.assertTrue(selected._closed)
            self.assertTrue(selected.retained_equipment)
            self.assertFalse(unused._closed)
            original_remove_row = unused._remove_row
            attempts = []

            def transient_remove(deadline):
                attempts.append(None)
                if len(attempts) == 1:
                    raise RuntimeError("synthetic transient row-removal failure")
                return original_remove_row(deadline)

            unused._remove_row = transient_remove
            member = object.__new__(AddedAmity)
            member.main, member.candidate = fixture.dut, fixture.member
            member.device_id, member.group = fixture.member.identity.device_id, fixture.source.group
            member._baseline = baseline
            member._request = lambda path, data=None, **kwargs: fixture.dut.request(path, data)
            member._removed_owners = lambda: set()
            later_checks = []

            def check_before_member_cleanup():
                later_checks.append(member._config(deadline=time.monotonic() + 1)["components"])

            bed.add_cleanup(check_before_member_cleanup)
        self.assertEqual(len(attempts), 2)
        self.assertEqual(later_checks, [baseline["components"]])
        self.assertEqual(fixture.full["components"], baseline["components"])
        self.assertEqual(selected.retained_equipment, [])
        self.assertEqual(bed.report["cleanup_errors"], [])
        self.assertEqual(len(fixture.rows), 1)

    def membership(self, *, permanent=False):
        fixture = MembershipFixture()
        fixture.main._testbed = fixture.candidate._testbed = None
        failures = [0]
        original_request = fixture.request

        def request(path, data=None, **kwargs):
            if path == "/api/config" and data is None and failures[0]:
                if not permanent:
                    failures[0] -= 1
                raise AmityHTTPError(409, "synthetic cleanup review failure")
            return original_request(path, data, **kwargs)

        fixture.main.request = fixture.request = request
        bed = TestBed(fixture.main, fixture.candidate, control_only=(fixture.candidate,), session_factory=UnitSession)
        return fixture, bed, failures

    def test_body_failure_survives_membership_cleanup_failure_and_successful_bed_retry(self):
        fixture, bed, failures = self.membership()
        original = TimeoutError("original activity failure")
        with self.assertRaises(TimeoutError) as raised:
            with bed:
                with fixture.main.add_amity(fixture.candidate):
                    failures[0] = 2
                    raise original
        self.assertIs(raised.exception, original)
        self.assertEqual(bed.report["error"], "TimeoutError")
        self.assertEqual(bed.report["cleanup_errors"], [])
        self.assertEqual(len(fixture.posts("remove")), 1)
        self.assertEqual(fixture.main.membership_operations[-1]["reset"], "scheduled")
        self.assertTrue(any(row.get("error_type") == "AmityHTTPError" for row in bed.report["memberships"]))

    def test_successful_body_still_fails_if_its_membership_context_cleanup_fails(self):
        fixture, bed, failures = self.membership()
        with self.assertRaises(MembershipCleanupError):
            with bed:
                with fixture.main.add_amity(fixture.candidate):
                    failures[0] = 1
        self.assertEqual(bed.report["error"], "MembershipCleanupError")
        self.assertEqual(bed.report["cleanup_errors"], [])
        self.assertEqual(fixture.main.membership_operations[-1]["reset"], "scheduled")

    def test_unresolved_bed_cleanup_still_fails_and_chains_the_original_body_failure(self):
        fixture, bed, failures = self.membership(permanent=True)
        original = TimeoutError("original source-theft failure")
        with self.assertRaises(TestBedCleanupError) as raised:
            with bed:
                with fixture.main.add_amity(fixture.candidate):
                    failures[0] = 1
                    raise original
        self.assertIs(raised.exception.__cause__, original)
        self.assertEqual(bed.report["error"], "TimeoutError")
        self.assertEqual(bed.report["cleanup_errors"], ["MembershipCleanupError"])
        self.assertEqual(fixture.posts("remove"), [])


if __name__ == "__main__":
    unittest.main()
