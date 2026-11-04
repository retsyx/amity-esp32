"""Direct DUT control using documented GPIO wiring and the shipped UI APIs.

Functional tests MUST use the same authenticated HTTP routes as Amity's UI.
Do not add firmware APIs, bypass authentication, or introduce test-only DUT
commands to support this framework. ``request`` is an escape hatch for existing
UI operations; all convenience methods below wrap those existing operations.
"""

import json
import logging
import math
from pathlib import Path
import re
import ssl
import stat
import subprocess
import time
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request
from uuid import uuid4

from .wiring import Wiring
from .config import current_config
from .trace import event
from .http_transport import HTTPSOpener


class AmityHTTPError(RuntimeError):
    def __init__(self, status, message, *, code=None):
        self.status = status
        self.code = code if isinstance(code, str) else None
        super().__init__(f"Amity HTTP {status}: {message}")


def _positive_seconds(value, description):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"{description} must be a positive, finite number of seconds")


def _http_trace_fields(path, data):
    """Select route/correlation metadata without recording HTTP contents."""
    fields = {}
    if path.startswith("/api/operations/"):
        route = "/api/operations/{operation_id}"
        identifier = path.removeprefix("/api/operations/")
        if len(identifier) == 32 and all(character in "0123456789abcdef" for character in identifier):
            fields["operation_id"] = identifier
    elif path.startswith("/api/devices/") and path.endswith("/settings"):
        route = "/api/devices/{device_id}/settings"
    elif path in {"/api/login", "/api/logout", "/api/system", "/api/config",
                  "/api/activity-editor", "/api/activity-editor/status",
                  "/api/activity-editor/activity", "/api/activity-editor/discover",
                  "/api/activity-editor/components",
                  "/api/key",
                  "/api/membership/admission", "/api/membership/scan", "/api/membership/add",
                  "/api/membership/remove", "/api/membership/cancel"}:
        route = path
    else:
        route = "/api/{route}"
    fields["route"] = route
    identifier = data.get("request_id") if isinstance(data, dict) else None
    if isinstance(identifier, str) and len(identifier) == 32 and all(character in "0123456789abcdef" for character in identifier):
        fields["request_id"] = identifier
    return fields


class PinctrlGPIO:
    """Pi GPIO control; release active-low inputs instead of driving them high."""

    def __init__(self, runner=subprocess.run):
        self.runner = runner

    def _set(self, gpio, *settings):
        self.runner(["pinctrl", "set", str(gpio), *settings], check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=1)

    def low(self, gpio):
        # Set the output latch before enabling output to avoid a high pulse.
        self._set(gpio, "dl")
        self._set(gpio, "op")

    def release(self, gpio):
        self._set(gpio, "ip", "pn")

    def hold_low(self, gpio, seconds):
        from .gpio import hold_low
        hold_low(self, gpio, seconds)


class DUT:
    """A named physical Amity, using its documented administration hostname.

    ``credentials`` is a private JSON file containing ``admin_password``.
    Authentication is explicit through ``login()`` (or automatic when a
    password/credential file is supplied). TLS verification is enabled unless
    ``insecure=True`` is explicitly chosen for a self-signed bench certificate.
    GPIO operations run on the Pi. HTTP operations can run anywhere with access
    to the DUT. The wiring inventory's optional hostname supplies the default
    HTTPS origin, falling back to the bench label. Override ``url`` as needed.
    Creating an instance without credentials performs no network
    operation and never presses a button or sends CEC.
    """

    def __init__(self, name, *, wiring=None, url=None, password=None, credentials=None,
                 gpio=None, cafile=None, insecure=None, opener=None):
        self._runtime = current_config()
        self._testbed = None
        self.last_scan = None
        self.activity_operations = []
        self.key_operations = []
        self.membership_operations = []
        if wiring is None:
            wiring = (Wiring.from_markdown(self._runtime.wiring_path)
                      if self._runtime.wiring_path is not None else Wiring.from_markdown())
        self.wiring = wiring
        if name not in self.wiring.identities:
            raise ValueError(f"Unknown DUT {name!r}")
        self.name = name
        self.identity = self.wiring.identities[name]
        self.gpio = (gpio if gpio is not None else
                     self._runtime.gpio_factory() if self._runtime.gpio_factory is not None else PinctrlGPIO())
        url = (self._runtime.admin_origins.get(name) or self._runtime.url_for(name)
               or f"https://{self.identity.hostname or name}.local") if url is None else url
        parsed = urlsplit(url)
        if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
            raise ValueError("DUT URL must be HTTPS without embedded credentials")
        if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
            raise ValueError("DUT URL must contain only the HTTPS origin")
        self.url = url.rstrip("/")
        if insecure is None:
            insecure = self._runtime.insecure if cafile is None else False
        if cafile is None and not insecure:
            cafile = self._runtime.cafile
        if cafile is not None and insecure:
            raise ValueError("Choose a CA certificate or insecure TLS, not both")
        context = ssl.create_default_context(cafile=str(cafile) if cafile else None)
        if insecure:
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
        self._ssl_context = context
        self._opener = opener if opener is not None else HTTPSOpener(self.url, context, dut=self.name)
        self._token = None
        if credentials is not None:
            if password is not None:
                raise ValueError("Provide password or credentials, not both")
            password = self._read_password(credentials)
        if password is not None:
            self.login(password)

    def tentacle(self, index=0, *, tv_input=1):
        """Return the documented front/back endpoints of one HDMI tentacle.

        This only resolves the parsed wiring lookup. It performs no network or
        hardware I/O and never changes DUT adapter settings.
        """
        from .connections import Tentacle
        return Tentacle(self, index, tv_input)

    def press_fw_reset(self, seconds):
        """Hold FW RESET active-low for ``seconds``, then release in all cases.

        The firmware decides the gesture: three to below ten seconds opens Add,
        while ten seconds requests factory reset. This method preserves those
        existing meanings and performs exactly the requested physical hold.
        """
        _positive_seconds(seconds, "FW RESET hold")
        gpio = self.wiring.reset_gpio(self.name)
        event("gpio.reset.start", dut=self.name, device_id=self.identity.device_id,
              gpio=gpio, hold_seconds=seconds)
        try:
            result = None
            if callable(getattr(self.gpio, "hold_low", None)):
                result = self.gpio.hold_low(gpio, seconds)
            else:
                try:
                    self.gpio.low(gpio)
                    time.sleep(seconds)
                finally:
                    self.gpio.release(gpio)
                    event("gpio.reset.released", dut=self.name, gpio=gpio)
            event("gpio.reset.complete", dut=self.name, gpio=gpio, released=True)
            return result
        except BaseException as error:
            event("gpio.reset.failed", level=logging.ERROR, dut=self.name, gpio=gpio,
                  error_type=type(error).__name__)
            raise

    def request(self, path, data=None, *, timeout=15):
        """GET or POST JSON to an existing UI endpoint, with bearer authentication."""
        _positive_seconds(timeout, "HTTP timeout")
        if not path.startswith("/api/") or "?" in path or "#" in path or ".." in path:
            raise ValueError("Use an existing absolute /api/ UI route")
        if path != "/api/login" and not self._token:
            raise RuntimeError("Log in before making authenticated DUT requests")
        headers = {"Accept": "application/json"}
        if self._token:
            headers["Authorization"] = "Bearer " + self._token
        body = None
        if data is not None:
            body = json.dumps(data, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = Request(self.url + path, headers=headers, data=body)
        trace = {"dut": self.name, "method": request.get_method(), **_http_trace_fields(path, data)}
        started = time.monotonic_ns()
        event("http.request.start", level=logging.DEBUG, **trace)
        try:
            with self._opener.open(request, timeout=timeout) as response:
                result = json.load(response)
                event("http.request.complete", level=logging.DEBUG, **trace,
                      status=getattr(response, "status", None),
                      duration_ms=round((time.monotonic_ns() - started) / 1_000_000, 3))
                return result
        except HTTPError as error:
            event("http.request.failed", level=logging.DEBUG, **trace, status=error.code,
                  error_type=type(error).__name__,
                  duration_ms=round((time.monotonic_ns() - started) / 1_000_000, 3))
            if error.code == 401:
                self._token = None
            try:
                payload = json.load(error)
                message = payload.get("error") or payload.get("message") or "Request failed"
                code = payload.get("code")
            except (ValueError, AttributeError):
                message = "Request failed"
                code = None
            finally:
                error.close()
            raise AmityHTTPError(error.code, message, code=code) from None
        except BaseException as error:
            event("http.request.failed", level=logging.DEBUG, **trace,
                  error_type=type(error).__name__,
                  duration_ms=round((time.monotonic_ns() - started) / 1_000_000, 3))
            raise

    def login(self, password):
        if not isinstance(password, str) or not password:
            raise ValueError("An admin password is required")
        # Discard a previous session before attempting a new login.
        self._token = None
        self.close()
        event("auth.login.start", dut=self.name, device_id=self.identity.device_id)
        try:
            response = self.request("/api/login", {"password": password})
            token = response.get("token") if isinstance(response, dict) else None
            if not isinstance(token, str) or not token:
                raise RuntimeError("Amity did not return an authenticated session")
            self._token = token
        except BaseException as error:
            try:
                self.close()
            except Exception as close_error:
                event("http.connection.close.failed", level=logging.ERROR, dut=self.name,
                      error_type=type(close_error).__name__)
            event("auth.login.failed", level=logging.ERROR, dut=self.name, error_type=type(error).__name__)
            raise
        event("auth.login.complete", dut=self.name)

    @staticmethod
    def _read_password(credentials):
        path = Path(credentials)
        if stat.S_IMODE(path.stat().st_mode) & 0o077:
            raise ValueError("DUT credential file must be private (mode 0600)")
        value = json.loads(path.read_text(encoding="utf-8"))
        password = value.get("admin_password") if isinstance(value, dict) else None
        if not isinstance(password, str) or not password:
            raise ValueError("Credential file must contain admin_password")
        return password

    def authenticate(self):
        """Use runner-provided credentials when a test bed opens this DUT."""
        if self._token:
            event("auth.session.reused", level=logging.DEBUG, dut=self.name)
            return
        credentials = self._runtime.credentials_for(self.name)
        if credentials is None:
            raise RuntimeError(f"Provide existing UI credentials for {self.name}")
        self.login(self._read_password(credentials))

    def scan_hdmi(self, *, timeout=None):
        """Run the UI's HDMI scan and return a verified framework scan result."""
        if self._testbed is None or self._testbed.session is None:
            raise RuntimeError("Use DUT.scan_hdmi() inside an active TestBed")
        from .discovery import scan_hdmi
        return scan_hdmi(self, self._testbed.session,
                         timeout=self._runtime.timeout if timeout is None else timeout)

    def create_activity(self, name, *, display, source, audio=None,
                        switch_device=None, switch_input=0, add_components=(), timeout=None):
        """Create an owned activity, optionally using the UI's Add component."""
        from .activity import Activity
        return Activity(self, name, display=display, source=source, audio=audio,
                        switch_device=switch_device, switch_input=switch_input,
                        add_components=add_components, timeout=timeout).create()

    def press_key(self, key, *, duration_ms=0):
        """Send a bounded remote press through the authenticated shipping API."""
        from .keys import press_key
        return press_key(self, key, duration_ms=duration_ms)

    def press_key_disconnected(self, key, component, *, duration_ms=3000):
        """Disconnect after physical key admission and verify bounded cleanup."""
        from .keys import press_key_disconnected
        return press_key_disconnected(self, key, component, duration_ms=duration_ms)

    def _disconnect_key_request(self, data, after_send, *, timeout):
        """Use the shipping key route without waiting for its HTTP response."""
        _positive_seconds(timeout, "HTTP timeout")
        if not self._token:
            raise RuntimeError("Log in before making authenticated DUT requests")
        disconnect = getattr(self._opener, "disconnect_after", None)
        if not callable(disconnect):
            raise RuntimeError("The HTTP transport cannot disconnect an in-flight request")
        request = Request(self.url + "/api/key", data=json.dumps(data, separators=(",", ":")).encode("utf-8"),
                          headers={"Accept": "application/json", "Content-Type": "application/json",
                                   "Authorization": "Bearer " + self._token})
        trace = {"dut": self.name, "method": "POST", "route": "/api/key"}
        event("http.request.start", level=logging.DEBUG, **trace)
        try:
            result = disconnect(request, after_send, timeout=timeout)
            event("http.request.disconnected", level=logging.DEBUG, **trace)
            return result
        except BaseException as error:
            event("http.request.failed", level=logging.DEBUG, **trace, error_type=type(error).__name__)
            raise

    def add_amity(self, candidate, *, name=None, timeout=None, setup_code=None):
        """Temporarily add a control-only DUT using the existing Add Amity UI.

        Closing the returned context removes that test-owned member and requires
        its acknowledgement of the UI's scheduled factory reset.
        """
        from .membership import AddedAmity
        return AddedAmity(self, candidate, name=name, timeout=timeout, setup_code=setup_code).create()

    def logout(self):
        event("auth.logout.start", dut=self.name, authenticated=bool(self._token))
        try:
            if self._token:
                self.request("/api/logout", {})
        except BaseException as error:
            event("auth.logout.failed", level=logging.ERROR, dut=self.name, error_type=type(error).__name__)
            raise
        else:
            event("auth.logout.complete", dut=self.name)
        finally:
            self._token = None
            self.close()

    def close(self):
        """Close this DUT's HTTP transport, including partially authenticated use."""
        close = getattr(getattr(self, "_opener", None), "close", None)
        if callable(close):
            close()

    def reconnect_hostname(self, hostname):
        """Follow a reviewed hostname edit with a fresh authenticated origin.

        No bearer token is forwarded across origins. The caller must probe
        the board identity before authenticating at the new name.
        """
        if not isinstance(hostname, str) or re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?", hostname) is None:
            raise ValueError("Use a valid Amity hostname")
        self.close()
        self._token = None
        self.url = f"https://{hostname}.local"
        self._opener = HTTPSOpener(self.url, self._ssl_context, dut=self.name)

    def probe_ready(self, *, timeout=15):
        """Read the UI's credential-free hostname reconnect identity proof."""
        _positive_seconds(timeout, "Readiness timeout")
        with self._opener.open(Request(self.url + "/api/ready", headers={"Accept": "application/json"}),
                               timeout=timeout) as response:
            return json.load(response)

    def get_state(self):
        return self.request("/api/activity-editor/status")

    def get_system(self):
        """Read the existing UI system snapshot, including this DUT's identity."""
        return self.request("/api/system")

    def front_physical_address(self, tentacle, address, *, timeout=None):
        """Temporarily edit this original main's adapter through the shipping UI."""
        from .settings import FrontPhysicalAddress
        return FrontPhysicalAddress.local(self, tentacle, address, timeout=timeout).apply()

    def pair_ble_keyboard(self, *, timeout=None):
        """Pair a test-owned BLE keyboard only when no remote is already bonded."""
        from .ble_keyboard import PairedKeyboard
        return PairedKeyboard(self, timeout=timeout).create()

    def capture_logs(self, *, expected_boot=None, timeout=15):
        """Read existing UI logs with before/after boot proof, without serial I/O."""
        from .diagnostics import capture_logs
        return capture_logs(self, expected_boot=expected_boot, timeout=timeout)

    def get_configuration(self):
        """Return the same activity editor document used by the ordinary UI."""
        return self.request("/api/activity-editor")

    def set_configuration(self, configuration, *, timeout=40):
        """Save an activity editor document using its existing revision checks.

        Supply the existing UI document, including its expected revisions and
        scoped device selections. No extra fixture configuration is persisted.
        """
        _positive_seconds(timeout, "Operation timeout")
        payload = dict(configuration)
        payload.pop("device_choices", None)
        payload["request_id"] = uuid4().hex
        response = self.request("/api/activity-editor", payload)
        return self.wait_operation(response, timeout=timeout)

    def wait_operation(self, operation, *, timeout=40, poll_interval=0.25):
        _positive_seconds(timeout, "Operation timeout")
        _positive_seconds(poll_interval, "Polling interval")
        pending = {"admitted", "preparing", "executing", "configuring", "activating", "stopping"}
        deadline = time.monotonic() + timeout
        stage = operation.get("stage")
        event("operation.stage", dut=self.name, operation_id=operation.get("operation_id"), stage=stage)
        while operation.get("stage") in pending:
            if time.monotonic() >= deadline:
                event("operation.timeout", level=logging.ERROR, dut=self.name,
                      operation_id=operation.get("operation_id"), stage=operation.get("stage"))
                raise TimeoutError("Amity UI operation did not complete")
            time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
            operation = self.request("/api/operations/" + operation["operation_id"])
            if operation.get("stage") != stage:
                stage = operation.get("stage")
                event("operation.stage", dut=self.name, operation_id=operation.get("operation_id"), stage=stage)
        if operation.get("stage") in {"failed", "rejected", "cancelled", "unknown"}:
            event("operation.failed", level=logging.ERROR, dut=self.name,
                  operation_id=operation.get("operation_id"), stage=operation.get("stage"))
            raise RuntimeError(operation.get("message") or "Amity UI operation failed")
        return operation

    def start_hdmi_scan(self):
        """Submit the UI's HDMI scan; its response acknowledges submission only."""
        return self.request("/api/activity-editor/discover", {"request_id": uuid4().hex})

    def wait_for(self, predicate, *, timeout=60, poll_interval=0.25):
        """Poll existing UI status until ``predicate(state)`` is true."""
        _positive_seconds(timeout, "Polling timeout")
        _positive_seconds(poll_interval, "Polling interval")
        deadline = time.monotonic() + timeout
        while True:
            state = self.get_state()
            if predicate(state):
                return state
            if time.monotonic() >= deadline:
                raise TimeoutError("Timed out waiting for Amity UI state")
            time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
