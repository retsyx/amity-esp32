"""A real, persistent browser session against the shipping administration UI.

Playwright is imported only when this opt-in fixture opens. Authentication uses
the visible password form; actions use normal controls and never inject state,
replace requests, or read application JavaScript variables.
"""

import base64
from collections import deque
import json
import math
import re
import shutil
import ssl
import time
from types import MappingProxyType
from urllib.parse import urlsplit

from .dut import _http_trace_fields
from .trace import event


_POLL_ROUTES = frozenset(("/api/activity-editor/status", "/api/remotes", "/api/matter",
                          "/api/homekit", "/api/system", "/api/config", "/api/updates/cohort"))
_NETWORK_RECOVERY_SECONDS = 30
_MAX_NETWORK_REQUESTS = 128
# A recovery can contain 120 operation receipt polls plus status/failure events.
_MAX_NETWORK_OBSERVATIONS = 256
_OPERATION_PENDING_STAGES = frozenset(("admitted", "preparing", "executing", "configuring",
                                       "activating", "stopping"))
_OPERATION_SUCCESS_STAGES = frozenset(("complete", "applied"))


def _network_time(value):
    return type(value) in (int, float) and math.isfinite(value) and value > 0


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
        self._cdp = None
        self._original_boot = None
        self._network_requests = {}
        self._network_observations = deque()
        self._draining_network = False
        self._network_body_read = None
        self._network_recovery = None
        self._network_recovery_error = None
        self._closing = False
        self._failures = []
        self._phase = "opening"
        self._last_status_fences = None
        self.report = {"selections": 0, "standbys": 0, "device_inspections": 0,
                       "page_errors": 0, "request_failures": 0, "http_errors": 0,
                       "network_interruptions": 0, "network_recoveries": 0}

    def _record_failure(self, kind, *, status=None, resource_type=None, fatal=True):
        if self._closing:
            return
        self.report[kind] += 1
        fields = {"kind": kind}
        if type(status) is int:
            fields["http_status"] = status
        if resource_type in {"document", "script", "stylesheet", "xhr", "fetch"}:
            fields["resource_type"] = resource_type
        if fatal and len(self._failures) < 16:
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
        boot = system.get("boot_id")
        if not isinstance(boot, str) or re.fullmatch(r"[0-9a-f]{32}", boot) is None:
            raise AssertionError("The browser has no verified original DUT boot")
        self._original_boot = boot
        hostname = system.get("hostname")
        if not isinstance(hostname, str) or not hostname:
            raise AssertionError("The main has no saved hostname for its rendered device row")
        proofs = self.main._testbed._member_proofs
        names = {owner.identity.device_id: (
            hostname if owner is self.main else
            proofs[owner.identity.device_id].name if owner.identity.device_id in proofs else owner.name)
            for owner in self._owners}
        self._owner_names = MappingProxyType(names)

    def _network_fault(self, reason, *, observed=None):
        if self._closing or self._network_recovery_error is not None:
            return
        timing = None
        pending = self._network_recovery
        if reason == "recovery_timeout" and pending is not None:
            timing = {"after": pending["after"], "deadline": pending["deadline"],
                      "interrupted_at": pending["deadline"] - _NETWORK_RECOVERY_SECONDS}
            if _network_time(observed):
                timing["observed_browser_timestamp"] = observed
            completions = []
            for kind, record, ended, _ in self._network_observations:
                operation = pending.get("operations", {}).get(record.get("operation_id"))
                cutoff = operation["after"] if operation is not None else pending["after"]
                if (kind == "complete" and record.get("status") == 200
                        and (operation is not None or record.get("route") == "/api/activity-editor/status")
                        and _network_time(record.get("started")) and _network_time(ended)
                        and cutoff < record["started"] <= ended <= pending["deadline"]):
                    completions.append((record["started"], ended))
            timing["queued_qualifying_completions"] = len(completions)
            if completions:
                timing["earliest_queued_start"] = min(started for started, _ in completions)
                timing["latest_queued_end"] = max(ended for _, ended in completions)
            timing["body_reads_in_progress"] = int(self._network_body_read is not None)
            if self._network_body_read is not None:
                started, ended = self._network_body_read
                if _network_time(started) and _network_time(ended):
                    timing.update(body_started=started, body_completed=ended)
            self.report["network_recovery_timing"] = timing
        self._network_recovery_error = reason
        self._network_recovery = None
        self.report["network_recovery_failure"] = reason
        if len(self._failures) < 16:
            self._failures.append({"kind": "network_recovery", "reason": reason})
        event("soak.browser.recovery_failed", dut=self.main.name, reason=reason,
              **({"timing": timing} if timing is not None else {}))

    def _queue_network_observation(self, kind, record, ended, identifier):
        if self._closing or self._network_recovery_error is not None:
            return
        if len(self._network_observations) >= _MAX_NETWORK_OBSERVATIONS:
            self._network_fault("observation_overflow")
            return
        self._network_observations.append((kind, record, ended, identifier))

    def _drain_network_observations(self):
        if self._draining_network:
            return
        self._draining_network = True
        try:
            while self._network_observations and not self._failures:
                kind, record, ended, identifier = self._network_observations.popleft()
                if kind == "failed":
                    self._observe_network_failure(record, ended)
                else:
                    self._observe_network_completion(record, ended, identifier)
        finally:
            self._draining_network = False

    def _network_started(self, parameters):
        """Keep only bounded CDP metadata; browser clocks survive Python work."""
        if self._closing:
            return
        identifier, request = parameters.get("requestId"), parameters.get("request", {})
        if not isinstance(identifier, str) or not 0 < len(identifier) <= 128 or not isinstance(request, dict):
            self._network_fault("invalid_request")
            return
        if identifier not in self._network_requests and len(self._network_requests) >= _MAX_NETWORK_REQUESTS:
            self._network_fault("request_overflow")
            return
        url = request.get("url")
        operation_id = None
        try:
            if not isinstance(url, str):
                raise ValueError()
            parsed, origin = urlsplit(url), urlsplit(self.main.url)
            same_origin = (parsed.scheme in {"http", "https"} and
                           (parsed.scheme, parsed.hostname, parsed.port or 443) ==
                           (origin.scheme, origin.hostname, origin.port or 443))
            operation = re.fullmatch(r"/api/operations/([0-9a-f]{32})", parsed.path)
            if same_origin and operation and not parsed.query and not parsed.fragment:
                operation_id = operation[1]
        except (TypeError, ValueError):
            same_origin = False
        resource_type = parameters.get("type")
        kind = ({"Document": "document", "Script": "script", "Stylesheet": "stylesheet",
                 "XHR": "xhr", "Fetch": "fetch"}.get(resource_type)
                if isinstance(resource_type, str) else None)
        method = request.get("method")
        self._network_requests[identifier] = {
            "route": _browser_request_route(url), "method": method if isinstance(method, str) else None,
            "resource_type": kind, "same_origin": same_origin,
            "started": parameters.get("timestamp"), "status": None}
        if operation_id is not None:
            # Exact operation identity is private recovery evidence, never a
            # request URL, response body, report field or logged identifier.
            self._network_requests[identifier]["operation_id"] = operation_id

    def _network_response(self, parameters):
        record = self._network_requests.get(parameters.get("requestId"))
        response = parameters.get("response")
        if record is not None and isinstance(response, dict):
            status = response.get("status")
            if type(status) in (int, float) and math.isfinite(status) and 100 <= status <= 599 and int(status) == status:
                record["status"] = int(status)

    def _network_failed(self, parameters):
        if self._closing:
            return
        record = self._network_requests.pop(parameters.get("requestId"), {})
        code = parameters.get("errorText")
        recoverable = (code == "net::ERR_NETWORK_CHANGED" and record.get("same_origin")
                       and record.get("method") == "GET" and record.get("resource_type") == "fetch"
                       and (record.get("route") in _POLL_ROUTES or record.get("operation_id") is not None))
        self._record_failure("request_failures", resource_type=record.get("resource_type"), fatal=not recoverable)
        evidence = {name: record[name] for name in ("route", "method", "resource_type")
                    if record.get(name) is not None and (name != "method" or record[name] in {"GET", "POST"})}
        evidence.setdefault("route", "/api/{route}")
        if self._phase in {"opening", "select", "standby", "devices"}:
            evidence["phase"] = self._phase
        if isinstance(code, str) and len(code) <= 96 and re.fullmatch(r"net::ERR_[A-Z0-9_]+", code):
            evidence["net_error"] = code
        if recoverable:
            evidence["recoverable"] = True
        failures = self.report.setdefault("failed_requests", [])
        if len(failures) < 16:
            failures.append(evidence)
        event("soak.browser.request_failed", dut=self.main.name, **evidence)
        if not recoverable:
            return
        self.report["network_interruptions"] += 1
        self._queue_network_observation("failed", record, parameters.get("timestamp"), None)

    def _observe_network_failure(self, record, ended):
        if self._network_recovery_error is not None:
            return
        started = record.get("started")
        if not _network_time(started) or not _network_time(ended) or ended < started:
            self._network_fault("invalid_timing")
        elif self._original_boot is None:
            self._network_fault("unverified_boot")
        elif self._network_recovery is None:
            self._network_recovery = {"after": ended, "deadline": ended + _NETWORK_RECOVERY_SECONDS}
        elif ended > self._network_recovery["deadline"]:
            self._network_fault("recovery_timeout", observed=ended)
        else:
            self._network_recovery["after"] = max(self._network_recovery["after"], ended)
        pending = self._network_recovery
        if pending is not None:
            if pending.get("health", {}).get("started", 0) <= pending["after"]:
                pending.pop("health", None)
            operation_id = record.get("operation_id")
            if operation_id is not None:
                operations = pending.setdefault("operations", {})
                if operation_id not in operations and len(operations) >= _MAX_NETWORK_REQUESTS:
                    self._network_fault("operation_overflow")
                    return
                # Rearm only this operation. A later unrelated poll failure
                # cannot require another read of an already completed action.
                operation = operations.setdefault(operation_id, {"after": ended, "completed": None})
                operation["after"] = max(operation["after"], ended)
                if operation.get("started", 0) <= operation["after"]:
                    operation["completed"] = None
                    operation.pop("started", None)

    def _complete_network_recovery(self, pending):
        if self._network_recovery is not pending or self._network_recovery_error is not None:
            return
        operations = pending.get("operations", {}).values()
        if "health" not in pending or any(operation["completed"] is None for operation in operations):
            return
        ended = max([pending["health"]["ended"], *(operation["completed"] for operation in operations)])
        self._network_recovery = None
        self.report["network_recoveries"] += 1
        event("soak.browser.network_recovered", dut=self.main.name,
              elapsed_seconds=round(ended - (pending["deadline"] - _NETWORK_RECOVERY_SECONDS), 3))

    def _network_finished(self, parameters):
        record = self._network_requests.pop(parameters.get("requestId"), {})
        if (self._closing or record.get("method") != "GET"
                or record.get("resource_type") != "fetch" or not record.get("same_origin")):
            return
        status_reply = record.get("route") == "/api/activity-editor/status"
        operation_id = record.get("operation_id")
        if not status_reply:
            pending_operations = (self._network_recovery or {}).get("operations", {})
            interrupted = operation_id is not None and (operation_id in pending_operations or any(
                kind == "failed" and failed.get("operation_id") == operation_id
                for kind, failed, _, _ in self._network_observations))
            if not interrupted:
                return
        if self._network_recovery is None and not any(kind == "failed" for kind, _, _, _ in self._network_observations):
            # Healthy receipt polling is not recovery evidence. Retain only the
            # latest status needed for conflict diagnostics during long actions.
            self._network_observations.clear()
        # Event fibers must not perform synchronous Playwright calls. A cached
        # body read yields that fiber, allowing a later deadline event to win
        # before a timely completion's proof resumes. Drain on the caller fiber.
        self._queue_network_observation("complete", record, parameters.get("timestamp"), parameters.get("requestId"))

    def _observe_preceding_failures(self, completed_at):
        if not _network_time(completed_at):
            return
        pending = self._network_recovery
        if pending is not None:
            proof_times = [pending.get("health", {}).get("ended"),
                           *(operation["completed"] for operation in pending.get("operations", {}).values())]
            completed_at = max([completed_at, *(value for value in proof_times if _network_time(value))])
        preceding, remaining = [], deque()
        for observation in self._network_observations:
            kind, record, ended, _ = observation
            if kind == "failed" and (not _network_time(ended) or ended <= completed_at):
                preceding.append((record, ended))
            else:
                remaining.append(observation)
        self._network_observations.clear()
        self._network_observations.extend(remaining)
        for record, ended in preceding:
            self._observe_network_failure(record, ended)

    def _completion_recovery_scope(self, record, ended):
        pending = self._network_recovery
        status_reply = record.get("route") == "/api/activity-editor/status"
        if pending is None:
            return None
        operation_id = record.get("operation_id")
        operation = pending.get("operations", {}).get(operation_id)
        if operation is None and not status_reply:
            return
        if record.get("status") != 200:
            if operation is not None:
                self._network_fault("invalid_operation")
            return
        started = record.get("started")
        if not _network_time(started) or not _network_time(ended) or ended < started:
            self._network_fault("invalid_timing")
            return
        if started <= (operation["after"] if operation is not None else pending["after"]):
            return
        if ended > pending["deadline"]:
            self._network_fault("recovery_timeout", observed=ended)
            return
        return pending, operation

    def _observe_network_completion(self, record, ended, identifier):
        self._observe_preceding_failures(ended)
        scope = self._completion_recovery_scope(record, ended)
        status_reply = record.get("route") == "/api/activity-editor/status"
        if self._failures or (scope is None and (self._network_recovery is not None
                                                or not status_reply or record.get("status") != 200)):
            return
        valid_body = True
        try:
            document = self._cached_network_document(record, ended, identifier)
        except Exception:
            valid_body = False
            document = None
        # Body IPC can pump an older failure notification. Apply failures that
        # ended before this completion before judging whether its GET is fresh.
        # A genuinely later failure remains queued for the next recovery window.
        self._observe_preceding_failures(ended)
        scope = self._completion_recovery_scope(record, ended)
        if status_reply:
            self._last_status_fences = (_activity_fences(document, status=True) if valid_body
                                        else {"invalid_fields": ["document"]})
        if scope is None or self._failures:
            return
        pending, operation = scope
        operation_id = record.get("operation_id")
        started = record["started"]
        if not valid_body:
            self._network_fault("invalid_operation" if operation is not None else "invalid_status")
            return
        try:
            if operation is not None:
                if (not isinstance(document, dict) or document.get("operation_id") != operation_id
                        or document.get("stage") not in _OPERATION_PENDING_STAGES | _OPERATION_SUCCESS_STAGES
                        or not isinstance(document.get("message"), str)
                        or "status" in document and document["status"] != document["stage"]):
                    self._network_fault("invalid_operation")
                    return
                if "boot_id" in document:
                    if not isinstance(document["boot_id"], str) or re.fullmatch(r"[0-9a-f]{32}", document["boot_id"]) is None:
                        self._network_fault("invalid_operation")
                        return
                    if document["boot_id"] != self._original_boot:
                        self._network_fault("changed_boot")
                        return
                if document["stage"] not in _OPERATION_SUCCESS_STAGES:
                    return
                if document["message"]:
                    self._network_fault("operation_failed")
                    return
            else:
                fences = _activity_fences(document, status=True)
                self._last_status_fences = fences
                required = {"expected_config_revision", "expected_state_revision", "expected_boot_id",
                            "current_activity", "activity_settling"}
                if set(fences) != required or document.get("ready") is not True:
                    self._network_fault("invalid_status")
                    return
                if fences["expected_boot_id"] != self._original_boot:
                    self._network_fault("changed_boot")
                    return
        except Exception:
            self._network_fault("invalid_operation" if operation is not None else "invalid_status")
            return
        # CDP sends pump callbacks. A newer interruption may have arrived while
        # reading this already completed response; its proof must stay pending.
        if self._network_recovery is not pending or self._network_recovery_error is not None:
            return
        if operation is not None:
            if pending.get("operations", {}).get(operation_id) is not operation or started <= operation["after"]:
                return
            operation["completed"] = ended
            operation["started"] = started
        else:
            if started <= pending["after"]:
                return
            pending["health"] = {"started": started, "ended": ended}
        self._complete_network_recovery(pending)

    def _cached_network_document(self, record, ended, identifier):
        self._network_body_read = (record.get("started"), ended)
        try:
            response = self._cdp.send("Network.getResponseBody", {"requestId": identifier})
        finally:
            self._network_body_read = None
        body = response["body"]
        if response.get("base64Encoded"):
            body = base64.b64decode(body, validate=True).decode("utf-8")
        return json.loads(body)

    def _await_network_recovery(self):
        safety_deadline = time.monotonic() + _NETWORK_RECOVERY_SECONDS
        while self._network_recovery is not None and not self._failures:
            try:
                response = self._cdp.send("Performance.getMetrics")
                self._drain_network_observations()
                if self._network_recovery is None or self._failures:
                    break
                metrics = response["metrics"]
                now = next(item["value"] for item in metrics if item.get("name") == "Timestamp")
                if not _network_time(now):
                    raise ValueError()
            except Exception:
                self._network_fault("invalid_clock")
                break
            remaining = self._network_recovery["deadline"] - now
            if remaining <= 0 or time.monotonic() >= safety_deadline:
                self._network_fault("recovery_timeout", observed=now)
                break
            self.page.wait_for_timeout(min(100, remaining * 1000))

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
            self.page.on("response", self._response)
            self._cdp = self._context.new_cdp_session(self.page)
            self._cdp.on("Network.requestWillBeSent", self._network_started)
            self._cdp.on("Network.responseReceived", self._network_response)
            self._cdp.on("Network.loadingFailed", self._network_failed)
            self._cdp.on("Network.loadingFinished", self._network_finished)
            self._cdp.send("Network.enable")
            self._cdp.send("Performance.enable", {"timeDomain": "timeTicks"})
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
        # Native Matter work can delay callbacks for longer than the recovery
        # budget. Drain queued failures and timely completions before looking at
        # the browser clock or starting a potentially long DOM assertion.
        if self._cdp is not None and self.page is not None and not self._failures and not self.page.is_closed():
            self.page.wait_for_timeout(0)
        self._drain_network_observations()
        if self._network_recovery is not None and not self._failures and self.page is not None:
            self._await_network_recovery()
        if self._failures:
            raise AssertionError("The soak browser observed a page, HTTP or network error")
        if self.page is None or self.page.is_closed():
            raise AssertionError("The soak browser page closed unexpectedly")

    def assert_ready(self):
        self.assert_healthy()
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
            row = root.locator("article").filter(
                has=self.page.get_by_role("heading", name=hostname, exact=True))
            self._expect(row).to_be_visible()
            status = "This device" if owner is self.main else "Connected"
            self._expect(row.get_by_text(status, exact=True)).to_be_visible()
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
            self._cdp = None
            self._network_requests.clear()
            self._network_observations.clear()

    def __exit__(self, exc_type, exc, traceback):
        try:
            if exc_type is None:
                self.assert_ready()
        finally:
            self.close()
        return False
