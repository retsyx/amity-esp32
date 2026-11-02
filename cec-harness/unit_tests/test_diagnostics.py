"""Authenticated text logs preserve identity evidence without serial access."""

import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT
from amity_harness.dut import AmityHTTPError


class Response(io.BytesIO):
    def __init__(self, data, content_type="text/plain; charset=utf-8"):
        super().__init__(data)
        self.content_type = content_type

    def getheader(self, name, default=None):
        return self.content_type if name.lower() == "content-type" else default


class Opener:
    def __init__(self, *responses):
        self.responses, self.requests, self.timeouts = list(responses), [], []

    def open(self, request, timeout):
        self.requests.append(request)
        self.timeouts.append(timeout)
        value = self.responses.pop(0)
        if isinstance(value, Exception):
            raise value
        return value


class DiagnosticsTests(unittest.TestCase):
    boot = "b" * 32

    def fixture(self, *, after_boot=None, expected_id=None, after_id=None, log_response=None):
        dut = DUT("amity-dut")
        before = {"octopus_id": expected_id or dut.identity.device_id, "boot_id": self.boot}
        after = {"octopus_id": after_id or dut.identity.device_id, "boot_id": after_boot or self.boot}
        logs = Response(b"W (17) cec: PRIVATE LOG\xff\n") if log_response is None else log_response
        opener = Opener(Response(json.dumps(before).encode(), "application/json"), logs,
                        Response(json.dumps(after).encode(), "application/json"))
        dut._opener, dut._token = opener, "PRIVATE_TOKEN"
        return dut, opener, logs

    def test_raw_text_download_uses_only_authenticated_existing_get_routes(self):
        dut, opener, logs = self.fixture()
        with patch("amity_harness.diagnostics.event") as event:
            capture = dut.capture_logs(expected_boot=self.boot)
        self.assertEqual(capture.data, b"W (17) cec: PRIVATE LOG\xff\n")
        self.assertIn("PRIVATE LOG", capture.text)
        self.assertIs(capture.assert_same_boot(), capture)
        self.assertEqual([request.full_url for request in opener.requests],
                         [dut.url + suffix for suffix in ("/api/system", "/api/logs", "/api/system")])
        self.assertTrue(all(request.get_method() == "GET" and request.data is None
                            and request.get_header("Authorization") == "Bearer PRIVATE_TOKEN"
                            for request in opener.requests))
        self.assertEqual(opener.requests[1].get_header("Accept"), "text/plain")
        self.assertTrue(logs.closed)
        self.assertNotIn("PRIVATE LOG", repr(capture))
        self.assertNotIn("PRIVATE_TOKEN", repr(capture))
        trace = str(event.call_args)
        self.assertNotIn("PRIVATE LOG", trace)
        self.assertNotIn("PRIVATE_TOKEN", trace)
        self.assertNotIn(dut.url, trace)
        self.assertEqual(capture.proof["bytes"], len(capture.data))
        self.assertLessEqual(capture.started_ns, capture.finished_ns)

    def test_concurrent_or_preexisting_restart_is_visible_without_discarding_logs(self):
        for after, expected in (("c" * 32, self.boot), (self.boot, "c" * 32)):
            with self.subTest(after=after, expected=expected):
                dut, _, _ = self.fixture(after_boot=after)
                capture = dut.capture_logs(expected_boot=expected)
                self.assertFalse(capture.same_boot)
                self.assertTrue(capture.data)
                self.assertEqual(capture.boot_before, self.boot)
                self.assertEqual(capture.boot_after, after)
                with self.assertRaisesRegex(AssertionError, "boot changed"):
                    capture.assert_same_boot()

    def test_identity_mismatch_before_capture_never_downloads_logs(self):
        dut, opener, _ = self.fixture(expected_id="e" * 32)
        with self.assertRaisesRegex(AssertionError, "matching Amity identity"):
            dut.capture_logs()
        self.assertEqual(len(opener.requests), 1)

    def test_identity_mismatch_after_capture_rejects_the_proof(self):
        dut, opener, logs = self.fixture(after_id="e" * 32)
        with self.assertRaisesRegex(AssertionError, "matching Amity identity"):
            dut.capture_logs()
        self.assertEqual(len(opener.requests), 3)
        self.assertTrue(logs.closed)

    def test_missing_authentication_and_invalid_parameters_perform_no_io(self):
        dut, opener, _ = self.fixture()
        dut._token = None
        with self.assertRaises(RuntimeError):
            dut.capture_logs()
        dut._token = "PRIVATE_TOKEN"
        for value in (0, -1, True, float("inf"), float("nan")):
            with self.subTest(timeout=value), self.assertRaises(ValueError):
                dut.capture_logs(timeout=value)
        for value in ("", "0" * 32, "wrong", 123):
            with self.subTest(boot=value), self.assertRaises(ValueError):
                dut.capture_logs(expected_boot=value)
        self.assertFalse(opener.requests)

    def test_plain_text_route_does_not_accept_json_or_missing_content_type(self):
        for content_type in ("application/json", ""):
            with self.subTest(content_type=content_type):
                dut, opener, logs = self.fixture(log_response=Response(b'{"error":"PRIVATE"}', content_type))
                with self.assertRaisesRegex(AssertionError, "plain text"):
                    dut.capture_logs()
                self.assertTrue(logs.closed)
                self.assertEqual(len(opener.requests), 2)

    def test_expired_session_during_log_download_closes_response_and_is_not_retried(self):
        error = HTTPError("https://amity-dut.local/api/logs", 401, "Unauthorized", {},
                          io.BytesIO(b"PRIVATE ERROR BODY"))
        dut, opener, _ = self.fixture(log_response=error)
        with self.assertRaises(AmityHTTPError) as failed:
            dut.capture_logs()
        self.assertEqual(failed.exception.status, 401)
        self.assertNotIn("PRIVATE", str(failed.exception))
        self.assertIsNone(dut._token)
        self.assertTrue(error.closed)
        self.assertEqual(len(opener.requests), 2)

    def test_one_total_deadline_is_passed_through_each_http_request(self):
        dut, opener, _ = self.fixture()
        with patch("amity_harness.diagnostics.time.monotonic", side_effect=(10, 11, 12, 13, 14)):
            dut.capture_logs(timeout=10)
        self.assertEqual(opener.timeouts, [9, 8, 7])

    def test_no_serial_device_or_output_file_is_opened(self):
        dut, _, _ = self.fixture()
        with patch("os.open", side_effect=AssertionError("A serial device was opened")), \
                patch("builtins.open", side_effect=AssertionError("An output file was opened")):
            dut.capture_logs(expected_boot=self.boot).assert_same_boot()

    def test_deadline_expiry_does_not_try_another_endpoint_or_transport(self):
        dut, opener, _ = self.fixture()
        with patch("amity_harness.diagnostics.time.monotonic", side_effect=(10, 11, 20)):
            with self.assertRaises(TimeoutError):
                dut.capture_logs(timeout=5)
        self.assertEqual(len(opener.requests), 1)


if __name__ == "__main__":
    unittest.main()
