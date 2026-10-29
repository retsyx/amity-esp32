"""Direct DUT control using documented GPIO wiring and the shipped UI APIs.

Functional tests MUST use the same authenticated HTTP routes as Amity's UI.
Do not add firmware APIs, bypass authentication, or introduce test-only DUT
commands to support this framework. ``request`` is an escape hatch for existing
UI operations; all convenience methods below wrap those existing operations.
"""

import json
import math
from pathlib import Path
import ssl
import stat
import subprocess
import time
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, HTTPSHandler
from uuid import uuid4

from .wiring import Wiring
from .config import current_config


class AmityHTTPError(RuntimeError):
    def __init__(self, status, message):
        self.status = status
        super().__init__(f"Amity HTTP {status}: {message}")


def _positive_seconds(value, description):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"{description} must be a positive, finite number of seconds")


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
    """A named physical Amity, with its HTTPS UI at ``https://{name}.local``.

    ``credentials`` is a private JSON file containing ``admin_password``.
    Authentication is explicit through ``login()`` (or automatic when a
    password/credential file is supplied). TLS verification is enabled unless
    ``insecure=True`` is explicitly chosen for a self-signed bench certificate.
    GPIO operations run on the Pi. HTTP operations can run anywhere with access
    to the DUT. Override ``url`` when its configured hostname differs from the
    bench label. Creating an instance without credentials performs no network
    operation and never presses a button or sends CEC.
    """

    def __init__(self, name, *, wiring=None, url=None, password=None, credentials=None,
                 gpio=None, cafile=None, insecure=None, opener=None):
        self._runtime = current_config()
        self._testbed = None
        self.last_scan = None
        self.activity_operations = []
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
        url = (self._runtime.url_for(name) or f"https://{name}.local") if url is None else url
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
        self._opener = opener if opener is not None else build_opener(HTTPSHandler(context=context))
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
        if callable(getattr(self.gpio, "hold_low", None)):
            return self.gpio.hold_low(gpio, seconds)
        try:
            self.gpio.low(gpio)
            time.sleep(seconds)
        finally:
            self.gpio.release(gpio)

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
        try:
            with self._opener.open(request, timeout=timeout) as response:
                return json.load(response)
        except HTTPError as error:
            if error.code == 401:
                self._token = None
            try:
                payload = json.load(error)
                message = payload.get("error") or payload.get("message") or "Request failed"
            except (ValueError, AttributeError):
                message = "Request failed"
            raise AmityHTTPError(error.code, message) from None

    def login(self, password):
        if not isinstance(password, str) or not password:
            raise ValueError("An admin password is required")
        # Discard a previous session before attempting a new login.
        self._token = None
        response = self.request("/api/login", {"password": password})
        token = response.get("token") if isinstance(response, dict) else None
        if not isinstance(token, str) or not token:
            raise RuntimeError("Amity did not return an authenticated session")
        self._token = token

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

    def create_activity(self, name, *, display, source, audio=None, timeout=None):
        """Create an owned temporary activity through the existing UI APIs."""
        from .activity import Activity
        return Activity(self, name, display=display, source=source, audio=audio, timeout=timeout).create()

    def add_amity(self, candidate, *, name=None, timeout=None, setup_code=None):
        """Temporarily add a control-only DUT using the existing Add Amity UI.

        Closing the returned context removes that test-owned member and requires
        its acknowledgement of the UI's scheduled factory reset.
        """
        from .membership import AddedAmity
        return AddedAmity(self, candidate, name=name, timeout=timeout, setup_code=setup_code).create()

    def logout(self):
        try:
            if self._token:
                self.request("/api/logout", {})
        finally:
            self._token = None

    def get_state(self):
        return self.request("/api/v2/activity-editor/status")

    def get_system(self):
        """Read the existing UI system snapshot, including this DUT's identity."""
        return self.request("/api/v2/system")

    def get_configuration(self):
        """Return the same activity editor document used by the ordinary UI."""
        return self.request("/api/v2/activity-editor")

    def set_configuration(self, configuration, *, timeout=40):
        """Save an activity editor document using its existing revision checks.

        Supply the existing UI document, including its expected revisions and
        scoped device selections. No extra fixture configuration is persisted.
        """
        _positive_seconds(timeout, "Operation timeout")
        payload = dict(configuration)
        payload.pop("device_choices", None)
        payload["request_id"] = uuid4().hex
        response = self.request("/api/v2/activity-editor", payload)
        return self.wait_operation(response, timeout=timeout)

    def wait_operation(self, operation, *, timeout=40, poll_interval=0.25):
        _positive_seconds(timeout, "Operation timeout")
        _positive_seconds(poll_interval, "Polling interval")
        pending = {"admitted", "preparing", "executing", "configuring", "activating", "stopping"}
        deadline = time.monotonic() + timeout
        while operation.get("stage") in pending:
            if time.monotonic() >= deadline:
                raise TimeoutError("Amity UI operation did not complete")
            time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
            operation = self.request("/api/v2/operations/" + operation["operation_id"])
        if operation.get("stage") in {"failed", "rejected", "cancelled", "unknown"}:
            raise RuntimeError(operation.get("message") or "Amity UI operation failed")
        return operation

    def start_hdmi_scan(self):
        """Submit the UI's HDMI scan; its response acknowledges submission only."""
        return self.request("/api/v2/activity-editor/discover", {"request_id": uuid4().hex})

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
