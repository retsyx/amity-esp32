"""Shared UI operations emit bounded, credential-free progress events."""

from pathlib import Path
import json
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_activity as activity_fixture
import test_discovery as discovery_fixture
import test_membership as membership_fixture
import test_scan_assertions as scan_fixture
from amity_harness.discovery import scan_hdmi


def events(mock):
    return [(call.args[0], call.kwargs) for call in mock.call_args_list]


class HelperLoggingTests(unittest.TestCase):
    def test_activity_reports_each_action_and_cleanup_with_identity(self):
        fixture = activity_fixture.Fixture()
        with patch("amity_harness.activity.event") as emit:
            activity = fixture.activity()
            activity.start()
            activity.assert_active()
            activity.standby()
            activity.close()
        captured = events(emit)
        names = [name for name, _ in captured]
        self.assertIn("activity.create.complete", names)
        self.assertIn("activity.assert_active.passed", names)
        self.assertEqual([fields["phase"] for name, fields in captured
                          if name == "activity.action.complete"], ["start", "standby"])
        self.assertLess(names.index("activity.create.submit"), names.index("activity.create.complete"))
        self.assertEqual(names[-1], "activity.cleanup.complete")
        for _, fields in captured:
            self.assertEqual(fields["device_id"], fixture.dut.identity.device_id)
            self.assertEqual(fields["boot"], fixture.dut.last_scan._boot)

    def test_activity_failure_logs_exception_type_without_exception_text(self):
        fixture = activity_fixture.Fixture()
        fixture.dut.get_configuration = lambda: (_ for _ in ()).throw(RuntimeError("PRIVATE_HTTP_DETAIL"))
        with patch("amity_harness.activity.event") as emit:
            with self.assertRaises(RuntimeError):
                fixture.activity()
        failed = [fields for name, fields in events(emit) if name == "activity.create.failed"]
        self.assertEqual(failed[0]["error_type"], "RuntimeError")
        self.assertNotIn("PRIVATE_HTTP_DETAIL", str(emit.call_args_list))

    def test_scan_logs_state_changes_once_and_records_verified_typed_location(self):
        fixture = scan_fixture.ScanAssertionTests()
        fixture.setUp()
        with patch("amity_harness.discovery.event") as emit:
            fixture.result._retain(fixture.result.status)
            fixture.result._retain(fixture.result.status)
            fixture.result.assert_on(fixture.tv, fixture.tv.endpoint)
        captured = events(emit)
        self.assertEqual(sum(name == "scan.state" for name, _ in captured), 1)
        location = next(fields for name, fields in captured if name == "scan.location.verified")
        self.assertEqual((location["owner_id"], location["tentacle"], location["side"]),
                         (fixture.dut.identity.device_id, 0, "front"))
        self.assertEqual(location["logical_address"], fixture.tv.address.logical)
        self.assertNotIn("must not enter locations", str(emit.call_args_list))

    def test_failed_scan_and_assertion_log_without_hiding_failure(self):
        dut = discovery_fixture.Device()
        dut.http_identity = "0" * 32
        with patch("amity_harness.discovery.event") as emit:
            with self.assertRaises(AssertionError):
                scan_hdmi(dut, dut.session)
            with self.assertRaises(AssertionError):
                dut.last_scan.assert_contains()
        self.assertEqual([name for name, _ in events(emit) if name.endswith("failed")],
                         ["scan.failed", "scan.assertion.failed"])

    def test_membership_logs_steps_and_reset_cleanup_without_admission_secrets(self):
        fixture = membership_fixture.Fixture()
        fixture.requires_code = True
        with patch("amity_harness.membership.event") as emit, \
                patch("amity_harness.membership.time.sleep"):
            member = fixture.added(setup_code=fixture.setup_code).create()
            member.close()
        captured = events(emit)
        names = [name for name, _ in captured]
        self.assertIn("membership.add.complete", names)
        self.assertIn("membership.assert_connected.passed", names)
        self.assertIn("membership.remove.reset_scheduled", names)
        self.assertEqual(names[-1], "membership.cleanup.complete")
        self.assertEqual([fields["step"] for name, fields in captured if name == "membership.step"],
                         ["preflight", "start_scan", "open_window", "discover_candidate",
                          "confirm_main", "submit_add", "confirm_add"])
        serialized = json.dumps(captured)
        self.assertNotIn(fixture.token, serialized)
        self.assertNotIn(fixture.setup_code, serialized)
        self.assertNotIn("candidate_token", serialized)

    def test_membership_repeated_polling_logs_only_changed_admission_phase(self):
        fixture = membership_fixture.Fixture()
        with patch("amity_harness.membership.event") as emit:
            member = fixture.added()
            member._observe_http("/api/membership/admission", lambda: {"phase": "scanning"})
            member._observe_http("/api/membership/admission", lambda: {"phase": "scanning"})
            member._observe_http("/api/membership/admission", lambda: {"phase": "found"})
        self.assertEqual([fields["phase"] for name, fields in events(emit)
                          if name == "membership.admission"], ["scanning", "found"])


if __name__ == "__main__":
    unittest.main()
