#!/usr/bin/env python3
"""Explicit real-Chromium checks for the soak browser, without any DUT access.

Run on the harness Pi with PYTHONPATH=cec-harness python3
cec-harness/browser_tests/soak_browser.py. This is separate from the ordinary
hardware-free unit suite and requires Playwright plus Chromium.
"""

from contextlib import contextmanager, redirect_stderr
from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import re
import secrets
import shutil
import ssl
import tempfile
from threading import Event, Thread
import time
from types import SimpleNamespace
import unittest
from urllib.request import Request, urlopen

from amity_harness.dut import DUT
from amity_harness.soak_browser import SoakBrowser
from amity_harness.trace import failure_fields, stdio_logging


ROOT = Path(__file__).resolve().parents[2]
MAIN_ID, GROUP_ID, BOOT_ID = (f"{number:032x}" for number in (1, 2, 3))
ACTIVITY_IDS = tuple(f"{number:032x}" for number in (11, 12))


class LocalInterface:
    def __init__(self, *, missing_activity=False, reject_login=False):
        self.password = secrets.token_hex(24)
        self.browser_token = secrets.token_hex(32)
        self.controller_token = secrets.token_hex(32)
        self.reject_login = reject_login
        self.requests = []
        self.activity_posts = []
        self.login_passwords = []
        self.opened_handles = None
        self.browser_fixture = None
        self.watch_requests = False
        self.unexpected_request = Event()
        self.owner_checks = 0
        self.state_revision = 0
        self.current_activity = -1
        self.names = ("amity", "amity-office", "amity-bedroom", "amity-media")
        self.identities = (MAIN_ID, *(f"{number:032x}" for number in (4, 5, 6)))
        self.activities = [dict(activity_id=identifier, name=name, display="", source="", audio="")
                           for identifier, name in zip(ACTIVITY_IDS, ("Morning source", "Evening source"))]
        self.visible_activities = self.activities[:1] if missing_activity else self.activities

    def system(self):
        return dict(octopus_id=MAIN_ID, consortium_id=GROUP_ID, boot_id=BOOT_ID,
                    hostname=self.names[0], mode="coordinator",
                    named_devices=[dict(octopus_id=owner, hostname=name)
                                   for owner, name in zip(self.identities, self.names)],
                    membership=dict(coordinator=True, authority_ready=True, member=False,
                                    membership_revision="1",
                                    peers=[dict(octopus_id=owner, hostname=name, boot_id=f"{number:032x}",
                                                enrolled=True, ready=True)
                                           for number, (owner, name) in enumerate(
                                               zip(self.identities[1:], self.names[1:]), 20)]))

    def endpoints(self):
        return {
            "/api/activity-editor": dict(version=1, expected_config_revision="7",
                                         expected_presentation_revision="3", device_choices=[],
                                         component_owners=[], activities=self.visible_activities),
            "/api/activity-editor/status": dict(
                version="browser-fixture", hostname=self.names[0], hardware="Amity",
                ready=True, activity_settling=False, discovering=False, discovery_generation=1,
                current_activity=self.current_activity, expected_config_revision="7",
                expected_presentation_revision="3", expected_state_revision=str(self.state_revision),
                expected_boot_id=BOOT_ID, devices=[], device_choices=[],
                wifi_connected=True, wifi_rssi=-40, wifi_channel=6, front_physical_address="1.0.0.0",
                front_address=4, back_address=0, tx_errors=0, rx_dropped=0),
            "/api/config": dict(version=3, controller_octopus_id=MAIN_ID,
                                config_revision="7", presentation_revision="3",
                                octopuses=[dict(octopus_id=owner, supported_tentacles=1)
                                           for owner in self.identities], activities=[]),
            "/api/system": self.system(),
            "/api/remotes": dict(devices=[], bonded=False, ready=True),
            "/api/matter": dict(fabrics=[], commissioning=False),
            "/api/homekit": dict(available=False, paired=False),
        }

    def validate_owners(self, components, *, _system):
        if (components != () or _system.get("octopus_id") != MAIN_ID
                or _system.get("boot_id") != BOOT_ID
                or _system.get("consortium_id") != GROUP_ID
                or {peer["octopus_id"] for peer in _system["membership"]["peers"]}
                != set(self.identities[1:])):
            raise AssertionError("The local browser fixture changed owner identity")
        self.owner_checks += 1

    def handler(self):
        fixture = self

        class Handler(SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(ROOT / "components/web_admin/assets"), **kwargs)

            def log_message(self, *args):
                pass

            def reply(self, value, status=200):
                data = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def record(self):
                if fixture.watch_requests:
                    fixture.unexpected_request.set()
                authenticated = self.headers.get("Authorization") in {
                    "Bearer " + fixture.controller_token, "Bearer " + fixture.browser_token}
                fixture.requests.append((self.command, self.path, authenticated))
                return authenticated

            def do_GET(self):
                authenticated = self.record()
                if self.path == "/api/setup":
                    return self.reply(dict(required=False, hostname=fixture.names[0]))
                if self.path.startswith("/api/"):
                    if not authenticated:
                        return self.reply(dict(error="Login required"), 401)
                    endpoints = fixture.endpoints()
                    if self.path in endpoints:
                        return self.reply(deepcopy(endpoints[self.path]))
                    return self.reply(dict(error="Unknown local fixture endpoint"), 404)
                super().do_GET()

            def do_POST(self):
                authenticated = self.record()
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                if self.path == "/api/login":
                    helper = fixture.browser_fixture
                    fixture.opened_handles = (helper._browser, helper.page, helper._playwright)
                    fixture.login_passwords.append(body.get("password"))
                    if fixture.reject_login or body.get("password") != fixture.password:
                        # Deliberate server text proves framework diagnostics do
                        # not copy arbitrary responses or credentials into logs.
                        return self.reply(dict(error="Rejected " + fixture.password), 401)
                    return self.reply(dict(token=fixture.browser_token))
                if not authenticated:
                    return self.reply(dict(error="Login required"), 401)
                if self.path != "/api/activity-editor/activity":
                    return self.reply(dict(error="Unexpected local fixture mutation"), 400)
                if (set(body) != {"request_id", "activity_id", "expected_config_revision",
                                 "expected_state_revision", "expected_boot_id"}
                        or not re.fullmatch("[0-9a-f]{32}", body.get("request_id", ""))
                        or body["expected_config_revision"] != "7"
                        or body["expected_state_revision"] != str(fixture.state_revision)
                        or body["expected_boot_id"] != BOOT_ID
                        or body["activity_id"] not in (*ACTIVITY_IDS, None)):
                    return self.reply(dict(error="The local activity fence changed"), 409)
                fixture.activity_posts.append(body)
                fixture.current_activity = (-1 if body["activity_id"] is None else
                                            ACTIVITY_IDS.index(body["activity_id"]))
                fixture.state_revision += 1
                return self.reply(dict(operation_id=body["request_id"], stage="complete"))

        return Handler


@contextmanager
def local_interface(**options):
    fixture = LocalInterface(**options)
    server = ThreadingHTTPServer(("127.0.0.1", 0), fixture.handler())
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            credentials = Path(directory) / "credentials.json"
            credentials.write_text(json.dumps(dict(admin_password=fixture.password)))
            credentials.chmod(0o600)
            main = SimpleNamespace(name="amity-dut", url=f"http://127.0.0.1:{server.server_port}",
                                   identity=SimpleNamespace(device_id=MAIN_ID, hostname="amity-dut"),
                                   _ssl_context=ssl.create_default_context(),
                                   _read_password=DUT._read_password,
                                   _runtime=SimpleNamespace(timeout=3, credentials_for=lambda name: credentials))
            owners = (main, *(SimpleNamespace(name=f"amity-dut-{number}",
                                             identity=SimpleNamespace(device_id=owner, hostname=f"amity-dut-{number}"))
                              for number, owner in enumerate(fixture.identities[1:], 2)))
            main._testbed = SimpleNamespace(duts=owners, _member_proofs={
                owner: SimpleNamespace(name=name)
                for owner, name in zip(fixture.identities[1:], fixture.names[1:])})
            main.last_scan = SimpleNamespace(expected=(), validate_owners=fixture.validate_owners)

            def get_system():
                request = Request(main.url + "/api/system",
                                  headers={"Authorization": "Bearer " + fixture.controller_token})
                with urlopen(request, timeout=3) as response:
                    return json.load(response)

            main.get_system = get_system
            fixture.main = main
            fixture.saved_activities = tuple(SimpleNamespace(dut=main, name=activity["name"])
                                             for activity in fixture.activities)
            yield fixture
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


class SoakBrowserIntegrationTests(unittest.TestCase):
    def assert_resources_closed(self, fixture):
        from playwright.sync_api import Error

        self.assertIsNotNone(fixture.opened_handles)
        chromium, page, driver = fixture.opened_handles
        self.assertTrue(page.is_closed())
        self.assertFalse(chromium.is_connected())
        helper = fixture.browser_fixture
        self.assertTrue(all(getattr(helper, name) is None
                            for name in ("page", "_context", "_browser", "_playwright")))
        # Browser.close alone leaves Playwright usable. A stopped driver must
        # reject a new browser launch through its public API as well.
        surprise_browser = None
        try:
            with self.assertRaisesRegex(
                    Error, r"(?i)(Playwright already stopped|event loop is closed|connection closed)"):
                executable = shutil.which("chromium") or shutil.which("chromium-browser")
                surprise_browser = driver.chromium.launch(
                    headless=True, **({"executable_path": executable} if executable else {}))
        finally:
            if surprise_browser is not None:
                surprise_browser.close()
                driver.stop()

    def test_real_login_activity_repair_standby_and_four_owner_inspection(self):
        with local_interface() as fixture:
            helper = fixture.browser_fixture = SoakBrowser(fixture.main, fixture.saved_activities)
            first, second = fixture.saved_activities
            with helper:
                self.assertEqual(fixture.owner_checks, 1)
                self.assertEqual(len(fixture.login_passwords), 1)
                self.assertTrue(fixture.login_passwords[0] == fixture.password)
                self.assertEqual(fixture.activity_posts, [])
                helper.inspect_devices()
                self.assertEqual(fixture.activity_posts, [])
                for activity in (first, first, second):
                    before = time.monotonic_ns()
                    boundary = helper.select(activity)
                    after = time.monotonic_ns()
                    self.assertTrue(before <= boundary <= after)
                    helper.assert_active(activity)
                self.assertEqual([request["activity_id"] for request in fixture.activity_posts],
                                 [ACTIVITY_IDS[0], ACTIVITY_IDS[0], ACTIVITY_IDS[1]])
                self.assertEqual([request["expected_state_revision"] for request in fixture.activity_posts],
                                 ["0", "1", "2"])
                before = time.monotonic_ns()
                boundary = helper.standby()
                self.assertTrue(before <= boundary <= time.monotonic_ns())
                self.assertIsNone(fixture.activity_posts[-1]["activity_id"])
                self.assertEqual(fixture.current_activity, -1)
                helper.inspect_devices()
                self.assertEqual(len(fixture.activity_posts), 4)
                self.assertEqual(helper.report, dict(selections=3, standbys=1, device_inspections=2,
                                                    page_errors=0, request_failures=0, http_errors=0))
                self.assertTrue(all(authenticated for _, path, authenticated in fixture.requests
                                    if path.startswith("/api/") and path not in ("/api/login", "/api/setup")))
                self.assertFalse(any(path.endswith("/discover") for _, path, _ in fixture.requests))
            self.assert_resources_closed(fixture)

    def test_failed_open_after_login_closes_page_browser_and_driver_and_stops_polling(self):
        with local_interface(missing_activity=True) as fixture:
            helper = fixture.browser_fixture = SoakBrowser(fixture.main, fixture.saved_activities)
            with self.assertRaises(AssertionError):
                helper.__enter__()
            self.assertEqual(len(fixture.login_passwords), 1)
            self.assertEqual(fixture.activity_posts, [])
            self.assert_resources_closed(fixture)
            fixture.watch_requests = True
            self.assertFalse(fixture.unexpected_request.wait(timeout=5),
                             "The closed browser continued polling its local fixture")

    def test_rejected_form_login_closes_resources_and_keeps_diagnostics_private(self):
        with local_interface(reject_login=True) as fixture:
            helper = fixture.browser_fixture = SoakBrowser(fixture.main, fixture.saved_activities)
            output = io.StringIO()
            with redirect_stderr(output), stdio_logging():
                try:
                    helper.__enter__()
                except AssertionError as error:
                    failure = failure_fields(error)
                else:
                    self.fail("A rejected form login must fail browser entry")
            self.assertEqual(helper.report["http_errors"], 1)
            self.assertEqual(fixture.activity_posts, [])
            self.assert_resources_closed(fixture)
            evidence = output.getvalue() + json.dumps(dict(report=helper.report, failure=failure))
            self.assertTrue(all(secret not in evidence for secret in
                                (fixture.password, fixture.browser_token, fixture.controller_token)))


if __name__ == "__main__":
    unittest.main()
