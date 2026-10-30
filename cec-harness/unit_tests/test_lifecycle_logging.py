"""Framework trace metadata and cleanup logging without physical DUT access."""

import io
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from amity_harness.dut import AmityHTTPError, DUT
from amity_harness.testbed import TestBed, TestBedCleanupError
from unit_tests.test_dut import Opener
from unit_tests.test_testbed import UnitDUT, UnitSession


class LifecycleLoggingTests(unittest.TestCase):
    def test_http_trace_excludes_credentials_contents_and_dynamic_route_values(self):
        secrets = ("private-password", "private-session-token", "private-origin.example",
                   "private-device-token", "private-request-token", "private-setup-code",
                   "private-discovery-token", "private-server-error", "private-response-value")
        error = HTTPError("https://private-origin.example/api/membership/add", 409, "Conflict", {},
                          io.BytesIO(b'{"error":"private-server-error"}'))
        opener = Opener({"token": "private-session-token"}, {"ready": True},
                        {"settings": "private-response-value"}, error)
        with patch("amity_harness.dut.event") as logged:
            dut = DUT("amity-dut", url="https://private-origin.example",
                      password="private-password", opener=opener)
            dut.get_state()
            dut.request("/api/devices/private-device-token/settings",
                        {"request_id": "private-request-token", "setup_code": "private-setup-code"})
            with self.assertRaises(AmityHTTPError):
                dut.request("/api/membership/add", {"token": "private-discovery-token"})
        captured = repr(logged.call_args_list)
        for secret in secrets:
            self.assertNotIn(secret, captured)
        records = [(call.args[0], call.kwargs) for call in logged.call_args_list]
        self.assertTrue(any(name == "http.request.complete"
                            and fields["route"] == "/api/devices/{device_id}/settings"
                            for name, fields in records))
        failures = [fields for name, fields in records if name == "http.request.failed"]
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["status"], 409)
        self.assertGreaterEqual(failures[0]["duration_ms"], 0)

    def test_operation_trace_reports_stage_changes_without_repeated_poll_noise(self):
        identity = "a" * 32
        dut = DUT("amity-dut")
        pending = {"operation_id": identity, "stage": "admitted"}
        replies = [pending, {**pending, "stage": "configuring"}, {**pending, "stage": "applied"}]
        with patch.object(dut, "request", side_effect=replies), \
                patch("amity_harness.dut.time.sleep"), patch("amity_harness.dut.event") as logged:
            result = dut.wait_operation(pending)
        stages = [call.kwargs["stage"] for call in logged.call_args_list if call.args[0] == "operation.stage"]
        self.assertEqual(stages, ["admitted", "configuring", "applied"])
        self.assertEqual(result["stage"], "applied")

    def test_interrupted_gpio_hold_logs_actual_release_and_failure(self):
        class GPIO:
            def low(self, pin):
                pass

            def release(self, pin):
                pass

        dut = DUT("amity-dut", gpio=GPIO())
        with patch("amity_harness.dut.time.sleep", side_effect=KeyboardInterrupt), \
                patch("amity_harness.dut.event") as logged, self.assertRaises(KeyboardInterrupt):
            dut.press_fw_reset(3.5)
        names = [call.args[0] for call in logged.call_args_list]
        self.assertEqual(names, ["gpio.reset.start", "gpio.reset.released", "gpio.reset.failed"])
        self.assertEqual(logged.call_args.kwargs["error_type"], "KeyboardInterrupt")

    def test_failed_cleanup_logs_every_attempt_without_exception_contents(self):
        operations = []
        dut = UnitDUT("first-board", operations, logout_error=True)
        bed = TestBed(dut, session_factory=lambda components: UnitSession(components, operations))

        def failed_cleanup():
            raise RuntimeError("private-cleanup-error")

        with patch("amity_harness.testbed.event") as logged, self.assertRaises(TestBedCleanupError):
            with bed:
                bed.add_cleanup(failed_cleanup)
        names = [call.args[0] for call in logged.call_args_list]
        self.assertIn("testbed.cleanup.callback.failed", names)
        self.assertIn("testbed.cleanup.cec.complete", names)
        self.assertIn("testbed.cleanup.logout.failed", names)
        self.assertIn("testbed.dut.released", names)
        self.assertIn("testbed.complete", names)
        self.assertNotIn("private-cleanup-error", repr(logged.call_args_list))
        self.assertNotIn("PRIVATE_LOGOUT_ERROR", repr(logged.call_args_list))
        self.assertIsNone(dut._testbed)
        self.assertFalse(bed.report["passed"])


if __name__ == "__main__":
    unittest.main()
