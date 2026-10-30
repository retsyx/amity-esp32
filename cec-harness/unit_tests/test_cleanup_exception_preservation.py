"""Context cleanup preserves the original failure and still retries/fails safely."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import TestBed
from amity_harness.activity import ActivityCleanupError
from amity_harness.dut import AmityHTTPError
from amity_harness.membership import MembershipCleanupError
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
        self.assertTrue(any(row["name"] == "Unused" and row.get("error_type") == "ActivityConflict" for row in cleanups))
        self.assertTrue(any(row["name"] == "Unused" and row.get("removed_activity") is True for row in cleanups))
        self.assertEqual(len(fixture.rows), 1)

    def test_successful_body_still_fails_if_its_activity_context_cleanup_fails(self):
        with self.assertRaises(ActivityCleanupError):
            with self.activities() as (fixture, bed):
                with ExitStack() as contexts:
                    selected = contexts.enter_context(fixture.activity("Selected"))
                    contexts.enter_context(fixture.activity("Unused", display=fixture.member_tv,
                        audio=fixture.member_avr, source=fixture.member_player))
                    selected.start()
        self.assertEqual(bed.report["error"], "ActivityCleanupError")
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
                    failures[0] = 1
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
