"""Browser fixture dependencies, failure evidence and resource cleanup."""

import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from amity_harness.soak_browser import SoakBrowser, _activity_fences, _browser_request_route


class SoakBrowserTests(unittest.TestCase):
    def fixture(self):
        main = SimpleNamespace(name="amity-dut", _testbed=SimpleNamespace(duts=()))
        main._testbed.duts = (main,)
        activity = SimpleNamespace(dut=main, name="Soak activity")
        return SoakBrowser(main, (activity,))

    def test_standby_observation_checks_rendered_state_without_submitting_another_action(self):
        browser = self.fixture()
        browser.page = Mock()
        browser._expect = Mock()
        browser.assert_ready = Mock()
        browser.assert_standby()
        self.assertEqual([call.args[0] for call in browser.page.locator.call_args_list],
                         ["#standby", "#activityList article.active"])
        browser._expect.return_value.to_be_hidden.assert_called_once_with()
        browser._expect.return_value.to_have_count.assert_called_once_with(0)
        browser.page.get_by_role.assert_not_called()
        browser.assert_ready.assert_called_once_with()

    def test_standby_observation_propagates_a_rendered_selection_failure(self):
        browser = self.fixture()
        browser.page = Mock()
        browser._expect = Mock()
        browser._expect.return_value.to_have_count.side_effect = AssertionError("Activity still selected")
        with self.assertRaisesRegex(AssertionError, "still selected"):
            browser.assert_standby()

    def test_failure_evidence_is_bounded_and_excludes_arbitrary_browser_text(self):
        browser = self.fixture()
        with patch("amity_harness.soak_browser.event") as trace:
            for _ in range(30):
                browser._record_failure("request_failures", resource_type="private-url-or-secret")
        self.assertEqual(browser.report["request_failures"], 30)
        self.assertEqual(len(browser._failures), 16)
        self.assertNotIn("private-url-or-secret", json.dumps(browser._failures))
        self.assertTrue(all(call.kwargs == {"dut": "amity-dut", "kind": "request_failures"}
                            for call in trace.call_args_list))
        with self.assertRaisesRegex(AssertionError, "page, HTTP or network"):
            browser.assert_healthy()

    def test_http_failure_observation_ignores_non_application_resources(self):
        browser = self.fixture()
        for kind, status in (("image", 404), ("fetch", 200), ("script", 200)):
            browser._response(SimpleNamespace(status=status, request=SimpleNamespace(resource_type=kind)))
        self.assertEqual(browser.report["http_errors"], 0)
        browser._response(SimpleNamespace(status=503, request=SimpleNamespace(resource_type="fetch")))
        self.assertEqual(browser._failures, [
            {"kind": "http_errors", "http_status": 503, "resource_type": "fetch"}])

    def test_network_failure_keeps_sanitized_route_method_code_and_phase_without_retry(self):
        browser = self.fixture()
        browser._phase = "select"
        request = SimpleNamespace(url="https://private-user:private-password@private.example/api/remotes?token=private-token",
                                  method="GET", resource_type="fetch", failure="net::ERR_CONNECTION_RESET",
                                  response=Mock(), headers={"Authorization": "Bearer private-token"})
        with patch("amity_harness.soak_browser.event") as trace:
            browser._request_failed(request)
        self.assertEqual(browser.report["request_failures"], 1)
        self.assertEqual(browser.report["failed_requests"], [{
            "route": "/api/remotes", "method": "GET", "resource_type": "fetch",
            "phase": "select", "net_error": "net::ERR_CONNECTION_RESET"}])
        request.response.assert_not_called()
        self.assertNotIn("private", json.dumps(browser.report))
        self.assertNotIn("private", str(trace.call_args_list))
        with self.assertRaisesRegex(AssertionError, "page, HTTP or network"):
            browser.assert_healthy()

    def test_network_failure_templates_drop_identifiers_and_malformed_urls_or_error_text(self):
        for url, route in (("https://private.example/api/operations/" + "a" * 32 + "?private-secret", "/api/operations/{operation_id}"),
                           ("https://private.example/api/devices/private-device/settings#private-secret", "/api/devices/{device_id}/settings"),
                           ("https://private.example/api/activity-editor/status?private-secret", "/api/activity-editor/status"),
                           ("https://[private-secret/api/system", "/api/{route}"),
                           ("private-secret", "/api/{route}"),
                           ("javascript:private-secret", "/api/{route}"),
                           ("https://private.example/private-secret", "/api/{route}"),
                           (None, "/api/{route}")):
            with self.subTest(url=url):
                self.assertEqual(_browser_request_route(url), route)
        for failure in (None, "private-secret", "net::ERR_FAILED private-secret",
                        "net::ERR_FAILED\nprivate-secret", "net::err_failed", "net::ERR_" + "A" * 100):
            with self.subTest(failure=failure):
                browser = self.fixture()
                browser._request_failed(SimpleNamespace(url="https://[private-secret", method="private-secret",
                                                        resource_type="private-secret", failure=failure))
                self.assertEqual(browser.report["failed_requests"], [{"route": "/api/{route}", "phase": "opening"}])
                self.assertNotIn("private-secret", json.dumps(browser.report))

    def test_network_failure_evidence_is_bounded_and_ignores_intentional_close(self):
        browser = self.fixture()
        request = SimpleNamespace(url="https://private.example/api/system", method="GET",
                                  resource_type="fetch", failure="net::ERR_ABORTED")
        self.assertNotIn("failed_requests", browser.report)
        for _ in range(30):
            browser._request_failed(request)
        self.assertEqual(browser.report["request_failures"], 30)
        self.assertEqual(len(browser.report["failed_requests"]), 16)
        self.assertEqual(len(browser._failures), 16)
        browser._closing = True
        browser._request_failed(request)
        self.assertEqual(browser.report["request_failures"], 30)
        self.assertEqual(len(browser.report["failed_requests"]), 16)

    def test_fence_sanitizer_accepts_zero_revision_and_rejects_untrusted_field_contents(self):
        valid = {"expected_config_revision": "18446744073709551615",
                 "expected_state_revision": "0", "expected_boot_id": "b" * 32,
                 "activity_id": None}
        self.assertEqual(_activity_fences({**valid, "password": "private-password",
                                          "url": "https://private.example"}), valid)
        invalid = _activity_fences({"expected_config_revision": "18446744073709551616",
                                   "expected_state_revision": True,
                                   "expected_boot_id": "private-token",
                                   "activity_id": "https://private.example"})
        self.assertEqual(invalid, {"invalid_fields": list(valid)})
        self.assertEqual(_activity_fences({"expected_state_revision": "00"}), {
            "invalid_fields": ["expected_state_revision"],
            "missing_fields": ["expected_config_revision", "expected_boot_id", "activity_id"]})
        self.assertEqual(_activity_fences("private-body"), {"invalid_fields": ["document"]})
        self.assertEqual(_activity_fences({**valid, "current_activity": True,
                                          "activity_settling": "private-state"}, status=True), {
            **{name: value for name, value in valid.items() if name != "activity_id"},
            "invalid_fields": ["current_activity", "activity_settling"]})

    def test_activity_conflict_records_only_failed_post_and_naturally_completed_status_fences(self):
        browser = self.fixture()
        observed = {"expected_config_revision": "7", "expected_state_revision": "0",
                    "expected_boot_id": "b" * 32, "current_activity": -1,
                    "activity_settling": False}
        status_json = Mock(return_value={**observed, "free_heap": 123456,
                                         "password": "private-status-secret"})
        status_request = SimpleNamespace(method="GET", url="https://private.example/api/activity-editor/status",
                                         response=lambda: SimpleNamespace(status=200, json=status_json))
        browser._request_finished(status_request)
        self.assertNotIn("last_failed_activity_request", browser.report)
        status_json.assert_called_once_with()
        browser._phase = "select"
        browser.report["device_inspections"] = 1
        request = SimpleNamespace(method="POST", resource_type="fetch",
                                  post_data_json={"expected_config_revision": "7", "expected_state_revision": "0",
                                                  "activity_id": "a" * 32, "password": "private-request-secret"})
        response = SimpleNamespace(status=409, request=request,
                                   url="https://private.example/api/activity-editor/activity", json=Mock())
        browser._response(response)
        evidence = browser.report["last_failed_activity_request"]
        self.assertEqual(evidence["phase"], "select")
        self.assertEqual(evidence["counters"]["device_inspections"], 1)
        self.assertEqual(evidence["counters"]["http_errors"], 1)
        self.assertEqual(evidence["request"], {
            "expected_config_revision": "7", "expected_state_revision": "0",
            "activity_id": "a" * 32, "missing_fields": ["expected_boot_id"]})
        self.assertEqual(evidence["latest_status"], observed)
        response.json.assert_not_called()
        serialized = json.dumps(browser.report)
        self.assertNotIn("private", serialized)
        self.assertNotIn("free_heap", serialized)

    def test_conflict_diagnostics_do_not_observe_other_routes_or_fetch_failed_response_body(self):
        browser = self.fixture()
        ignored = SimpleNamespace(method="GET", url="https://private.example/api/system", response=Mock())
        browser._request_finished(ignored)
        ignored.response.assert_not_called()
        response = SimpleNamespace(status=409,
                                   request=SimpleNamespace(method="POST", resource_type="fetch"),
                                   url="https://private.example/api/config", json=Mock())
        browser._response(response)
        self.assertNotIn("last_failed_activity_request", browser.report)
        response.json.assert_not_called()

    def test_device_headings_use_saved_main_hostname_and_committed_member_name(self):
        browser = self.fixture()
        main = browser.main
        main.identity = SimpleNamespace(device_id="main-id", hostname="amity-dut")
        member = SimpleNamespace(name="amity-dut-2", identity=SimpleNamespace(device_id="member-id"))
        browser._owners = (main, member)
        main._testbed._member_proofs = {"member-id": SimpleNamespace(name="living-room")}
        system = {"hostname": "amity"}
        main.get_system = Mock(return_value=system)
        main.last_scan = SimpleNamespace(expected=(), validate_owners=Mock())
        browser._capture_owner_names()
        self.assertEqual(dict(browser._owner_names), {"main-id": "amity", "member-id": "living-room"})
        main.last_scan.validate_owners.assert_called_once_with((), _system=system)
        system["hostname"] = "changed-after-browser-open"
        browser._capture_owner_names()
        main.get_system.assert_called_once_with()
        self.assertEqual(browser._owner_names["main-id"], "amity")
        with self.assertRaises(TypeError):
            browser._owner_names["main-id"] = "amity-dut"

    def test_missing_browser_dependency_fails_clearly_without_import_time_requirement(self):
        browser = self.fixture()
        browser.main._runtime = SimpleNamespace(credentials_for=lambda name: "/private/credentials.json")
        browser.main._read_password = lambda credentials: "private-password"
        with patch.dict("sys.modules", {"playwright": None, "playwright.sync_api": None}), \
                self.assertRaisesRegex(RuntimeError, "Playwright and Chromium on the harness Pi"):
            browser.__enter__()
        self.assertIsNone(browser._playwright)

    def test_page_closure_is_an_explicit_failure(self):
        browser = self.fixture()
        browser.page = SimpleNamespace(is_closed=lambda: True)
        with self.assertRaisesRegex(AssertionError, "closed unexpectedly"):
            browser.assert_healthy()

    def test_exit_closes_browser_and_driver_even_when_browser_health_failed(self):
        browser = self.fixture()
        chromium, driver = Mock(), Mock()
        browser._browser, browser._playwright = chromium, driver
        browser._record_failure("page_errors")
        with self.assertRaises(AssertionError):
            browser.__exit__(None, None, None)
        chromium.close.assert_called_once_with()
        driver.stop.assert_called_once_with()
        self.assertIsNone(browser.page)
        browser._record_failure("request_failures")
        self.assertEqual(browser.report["request_failures"], 0)


if __name__ == "__main__":
    unittest.main()
