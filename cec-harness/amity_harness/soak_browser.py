"""A real, persistent browser session against the shipping administration UI.

Playwright is imported only when this opt-in fixture opens. Authentication uses
the visible password form; actions use normal controls and never inject state,
replace requests, or read application JavaScript variables.
"""

import re
import shutil
import ssl
import time
from types import MappingProxyType
from urllib.parse import urlsplit

from .dut import _http_trace_fields
from .trace import event


def _activity_fences(document, *, status=False):
    """Select typed concurrency evidence without retaining other HTTP data."""
    required = ("expected_config_revision", "expected_state_revision", "expected_boot_id")
    fields = required + (("current_activity", "activity_settling") if status else ("activity_id",))
    if not isinstance(document, dict):
        return {"invalid_fields": ["document"]}
    selected, invalid = {}, []
    for name in fields:
        if name not in document:
            continue
        value = document[name]
        if name.endswith("revision"):
            valid = (isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]{0,19}", value)
                     and int(value) <= 0xffffffffffffffff)
        elif name in {"expected_boot_id", "activity_id"}:
            valid = ((name == "activity_id" and value is None)
                     or isinstance(value, str) and re.fullmatch(r"[0-9a-f]{32}", value))
        elif name == "current_activity":
            valid = type(value) is int and -1 <= value < 16
        else:
            valid = type(value) is bool
        if valid:
            selected[name] = value
        else:
            invalid.append(name)
    if invalid:
        selected["invalid_fields"] = invalid
    missing = [name for name in required + (() if status else ("activity_id",)) if name not in document]
    if missing:
        selected["missing_fields"] = missing
    return selected


def _browser_request_route(url):
    """Keep a fixed UI route or template, without its origin or identifiers."""
    try:
        parsed = urlsplit(url) if isinstance(url, str) else None
        if parsed is None or parsed.scheme not in {"http", "https"} or not parsed.netloc:
            return "/api/{route}"
        path = parsed.path
    except ValueError:
        return "/api/{route}"
    if path in {"/api/setup", "/api/ready", "/api/remotes", "/api/matter",
                "/api/homekit", "/api/updates/cohort", "/api/logs"}:
        return path
    return _http_trace_fields(path, None)["route"]


class SoakBrowser:
    """Keep Chromium and the main's actual UI open throughout a soak run."""

    def __init__(self, main, saved_activities):
        self.main = main
        self.activities = tuple(saved_activities)
        if not self.activities or any(activity.dut is not main for activity in self.activities):
            raise ValueError("Browser activities must belong to the main DUT")
        self._owners = tuple(main._testbed.duts)
        self._owner_names = None
        self._playwright = self._browser = self._context = self.page = None
        self._closing = False
        self._failures = []
        self._phase = "opening"
        self._last_status_fences = None
        self.report = {"selections": 0, "standbys": 0, "device_inspections": 0,
                       "page_errors": 0, "request_failures": 0, "http_errors": 0}

    def _record_failure(self, kind, *, status=None, resource_type=None):
        if self._closing:
            return
        self.report[kind] += 1
        fields = {"kind": kind}
        if type(status) is int:
            fields["http_status"] = status
        if resource_type in {"document", "script", "stylesheet", "xhr", "fetch"}:
            fields["resource_type"] = resource_type
        if len(self._failures) < 16:
            self._failures.append(fields)
        # Browser exception messages, request URLs, headers and bodies can hold
        # credentials or server text. Only counts and resource kinds enter logs.
        event("soak.browser.failed", dut=self.main.name, **fields)

    def _capture_owner_names(self):
        if self._owner_names is not None:
            return
        system = self.main.get_system()
        scan = self.main.last_scan
        scan.validate_owners(scan.expected, _system=system)
        hostname = system.get("hostname")
        if not isinstance(hostname, str) or not hostname:
            raise AssertionError("The main has no saved hostname for its rendered device row")
        proofs = self.main._testbed._member_proofs
        names = {owner.identity.device_id: (
            hostname if owner is self.main else
            proofs[owner.identity.device_id].name if owner.identity.device_id in proofs else owner.name)
            for owner in self._owners}
        self._owner_names = MappingProxyType(names)

    def _request_failed(self, request):
        """Record the original browser failure with strictly selected metadata."""
        if self._closing:
            return
        resource = getattr(request, "resource_type", None)
        self._record_failure("request_failures", resource_type=resource)
        evidence = {"route": _browser_request_route(getattr(request, "url", None))}
        method = getattr(request, "method", None)
        if method in {"GET", "POST"}:
            evidence["method"] = method
        if resource in {"document", "script", "stylesheet", "xhr", "fetch"}:
            evidence["resource_type"] = resource
        if self._phase in {"opening", "select", "standby", "devices"}:
            evidence["phase"] = self._phase
        failure = getattr(request, "failure", None)
        if (isinstance(failure, str) and len(failure) <= 96
                and re.fullmatch(r"net::ERR_[A-Z0-9_]+", failure)):
            evidence["net_error"] = failure
        failures = self.report.setdefault("failed_requests", [])
        if len(failures) < 16:
            failures.append(evidence)
        event("soak.browser.request_failed", dut=self.main.name, **evidence)

    def _response(self, response):
        if self._closing:
            return
        kind = response.request.resource_type
        if response.status >= 400 and kind in {"document", "script", "stylesheet", "xhr", "fetch"}:
            self._record_failure("http_errors", status=response.status, resource_type=kind)
            if response.status == 409 and self._activity_response(response):
                try:
                    document = response.request.post_data_json
                except Exception:
                    document = None
                evidence = {"http_status": 409, "phase": self._phase,
                            "counters": {name: self.report[name] for name in
                                         ("selections", "standbys", "device_inspections", "page_errors",
                                          "request_failures", "http_errors")},
                            "request": _activity_fences(document)}
                if self._last_status_fences is not None:
                    evidence["latest_status"] = dict(self._last_status_fences)
                self.report["last_failed_activity_request"] = evidence

    def _request_finished(self, request):
        """Remember only naturally completed status replies, without waiting."""
        if (self._closing or request.method != "GET"
                or urlsplit(request.url).path != "/api/activity-editor/status"):
            return
        try:
            response = request.response()
            if response is not None and response.status == 200:
                self._last_status_fences = _activity_fences(response.json(), status=True)
        except Exception:
            # Diagnostic extraction cannot replace the browser's actual result.
            # No exception text or response body is retained.
            self._last_status_fences = {"invalid_fields": ["document"]}

    def __enter__(self):
        credentials = self.main._runtime.credentials_for(self.main.name)
        if credentials is None:
            raise RuntimeError("The soak browser requires the main's private UI credentials")
        password = self.main._read_password(credentials)
        try:
            from playwright.sync_api import expect, sync_playwright
        except ImportError:
            raise RuntimeError("The soak test requires Playwright and Chromium on the harness Pi") from None
        self._expect = expect
        expect.set_options(timeout=self.main._runtime.timeout * 1000)
        try:
            self._capture_owner_names()
            self._playwright = sync_playwright().start()
            executable = shutil.which("chromium") or shutil.which("chromium-browser")
            self._browser = self._playwright.chromium.launch(
                headless=True, **({"executable_path": executable} if executable else {}))
            self._context = self._browser.new_context(
                ignore_https_errors=self.main._ssl_context.verify_mode == ssl.CERT_NONE,
                viewport={"width": 1280, "height": 900})
            self.page = self._context.new_page()
            self.page.set_default_timeout(self.main._runtime.timeout * 1000)
            self.page.on("pageerror", lambda error: self._record_failure("page_errors"))
            self.page.on("requestfailed", self._request_failed)
            self.page.on("response", self._response)
            self.page.on("requestfinished", self._request_finished)
            self.page.goto(self.main.url, wait_until="domcontentloaded")
            self._expect(self.page.locator("#login")).to_be_visible()
            self.page.get_by_label("Password", exact=True).fill(password)
            self.page.locator("#loginForm").get_by_role("button", name="Log in", exact=True).click()
            self.assert_ready()
            self._expect(self.page.locator("#activityList article")).to_have_count(len(self.activities))
            for activity in self.activities:
                self._expect(self._row(activity)).to_be_visible()
            event("soak.browser.open", dut=self.main.name, activities=len(self.activities))
            return self
        except BaseException:
            self.close()
            raise
        finally:
            password = None

    def _row(self, activity):
        if not any(activity is saved for saved in self.activities):
            raise ValueError("The browser activity is outside this soak fixture")
        return self.page.locator("#activityList article").filter(
            has=self.page.get_by_text(activity.name, exact=True))

    def assert_healthy(self):
        if self._failures:
            raise AssertionError("The soak browser observed a page, HTTP or network error")
        if self.page is None or self.page.is_closed():
            raise AssertionError("The soak browser page closed unexpectedly")

    def assert_ready(self):
        self._expect(self.page.locator("#admin")).to_be_visible()
        self._expect(self.page.locator("#login")).to_be_hidden()
        self._expect(self.page.locator("#connection")).to_have_text("")
        self._expect(self.page.locator("#activityList [data-start]").first).to_be_enabled()
        self._expect(self.page.locator("#message")).not_to_have_class(re.compile(r"\berror\b"))
        self.assert_healthy()

    def assert_active(self, activity):
        self.assert_ready()
        self._expect(self.page.locator("#activityList article.active")).to_have_count(1)
        row = self._row(activity)
        self._expect(row).to_have_class(re.compile(r"\bactive\b"))
        self._expect(row.locator(".activity-badge")).to_be_visible()
        self._expect(row.locator("[data-start]")).to_have_text("Repair")
        self.assert_healthy()

    @staticmethod
    def _activity_response(response):
        return (response.request.method == "POST"
                and urlsplit(response.url).path == "/api/activity-editor/activity")

    def select(self, activity):
        """Click Start/Repair and return the fresh physical-command boundary."""
        self._phase = "select"
        self.page.get_by_role("button", name="Activities", exact=True).click()
        self.assert_ready()
        row = self._row(activity)
        started_ns = time.monotonic_ns()
        with self.page.expect_response(self._activity_response) as response:
            row.locator("[data-start]").click()
        if not response.value.ok:
            raise AssertionError("The browser activity selection failed")
        self.assert_active(activity)
        self.report["selections"] += 1
        event("soak.browser.activity", dut=self.main.name, activity=activity.name,
              started_monotonic_ns=started_ns)
        return started_ns

    def standby(self):
        """Click Standby and wait for the rendered selection to clear."""
        self._phase = "standby"
        self.page.get_by_role("button", name="Activities", exact=True).click()
        self.assert_ready()
        started_ns = time.monotonic_ns()
        with self.page.expect_response(self._activity_response) as response:
            self.page.locator("#standby").click()
        if not response.value.ok:
            raise AssertionError("The browser standby selection failed")
        self.assert_standby()
        self.report["standbys"] += 1
        event("soak.browser.standby", dut=self.main.name, started_monotonic_ns=started_ns)
        return started_ns

    def assert_standby(self):
        """Observe rendered standby after either UI or Matter selection."""
        self._expect(self.page.locator("#standby")).to_be_hidden()
        self._expect(self.page.locator("#activityList article.active")).to_have_count(0)
        self.assert_ready()

    def inspect_devices(self):
        """Inspect all owners and member connectivity in the rendered device list."""
        self._phase = "devices"
        self.page.get_by_role("button", name="Amity devices", exact=True).click()
        root = self.page.locator("#managedAmityList")
        self._expect(root).to_be_visible()
        self._expect(root.locator("article")).to_have_count(len(self._owners))
        for owner in self._owners:
            hostname = self._owner_names[owner.identity.device_id]
            heading = hostname + " (this device)" if owner is self.main else hostname
            row = root.locator("article").filter(
                has=self.page.get_by_role("heading", name=heading, exact=True))
            self._expect(row).to_be_visible()
            if owner is not self.main:
                self._expect(row.get_by_text("Connected", exact=True)).to_be_visible()
        self.page.get_by_role("button", name="Activities", exact=True).click()
        self.assert_ready()
        self.report["device_inspections"] += 1
        event("soak.browser.devices", dut=self.main.name, owners=len(self._owners))

    def close(self):
        self._closing = True
        try:
            if self._browser is not None:
                self._browser.close()
        finally:
            if self._playwright is not None:
                self._playwright.stop()
            self.page = self._context = self._browser = self._playwright = None

    def __exit__(self, exc_type, exc, traceback):
        try:
            if exc_type is None:
                self.assert_healthy()
        finally:
            self.close()
        return False
