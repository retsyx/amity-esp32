"""Read-only firmware logs through the shipping UI, without opening USB serial.

The ESP32-S3 native USB Serial/JTAG interface interprets DTR/RTS sequences as
reset commands. Setting both False before pyserial.open() is not a passive
capture guarantee: open/configuration and separate modem-control writes can
pass through a resetting state. Use this authenticated text download when
diagnostics must preserve the current boot.
"""

from dataclasses import dataclass, field
import time
from urllib.error import HTTPError
from urllib.request import Request

from .discovery import _identifier
from .dut import AmityHTTPError, _positive_seconds
from .trace import event


@dataclass(frozen=True)
class LogCapture:
    """Exact log bytes plus identity/boot evidence around their download.

    Logs can contain private information. They are excluded from repr and
    framework traces; the caller chooses where to save ``data``.
    """

    data: bytes = field(repr=False)
    device_id: str
    boot_before: str
    boot_after: str
    expected_boot: str | None
    started_ns: int
    finished_ns: int

    @property
    def text(self):
        return self.data.decode("utf-8", errors="replace")

    @property
    def same_boot(self):
        return (self.boot_before == self.boot_after
                and (self.expected_boot is None or self.boot_before == self.expected_boot))

    @property
    def proof(self):
        """Credential-free proof suitable for a separate JSON evidence file."""
        return {"device_id": self.device_id, "boot_before": self.boot_before,
                "boot_after": self.boot_after, "expected_boot": self.expected_boot,
                "same_boot": self.same_boot, "started_monotonic_ns": self.started_ns,
                "finished_monotonic_ns": self.finished_ns, "bytes": len(self.data)}

    def assert_same_boot(self):
        """Reject a concurrent restart while keeping captured logs available."""
        if not self.same_boot:
            raise AssertionError("The Amity boot changed before or during log capture")
        return self


def capture_logs(dut, *, expected_boot=None, timeout=15):
    """Download existing authenticated ``GET /api/logs`` as plain text.

    Before/after reads use the existing system endpoint and the same selected
    TLS transport. There is no serial fallback, mutation, session retry or new
    firmware endpoint. The returned proof exposes any concurrent restart; use
    ``assert_same_boot()`` when boot preservation is required.
    """
    _positive_seconds(timeout, "Log capture timeout")
    if expected_boot is not None and not _identifier(expected_boot):
        raise ValueError("Log capture needs a valid expected boot identity")
    if not dut._token:
        raise RuntimeError("Log in before capturing authenticated DUT logs")
    started = time.monotonic_ns()
    deadline = time.monotonic() + timeout

    def remaining():
        value = deadline - time.monotonic()
        if value <= 0:
            raise TimeoutError("The log capture deadline expired")
        return value

    def identity():
        system = dut.request("/api/system", timeout=remaining())
        if (not isinstance(system, dict) or system.get("octopus_id") != dut.identity.device_id
                or not _identifier(system.get("boot_id"))):
            raise AssertionError("Log capture has no matching Amity identity and boot proof")
        return system["boot_id"]

    before = identity()
    request = Request(dut.url + "/api/logs", headers={"Accept": "text/plain",
                      "Authorization": "Bearer " + dut._token})
    try:
        with dut._opener.open(request, timeout=remaining()) as response:
            content_type = response.getheader("Content-Type", "").split(";", 1)[0].strip().lower()
            if content_type != "text/plain":
                raise AssertionError("The firmware log endpoint did not return plain text")
            data = response.read()
    except HTTPError as error:
        if error.code == 401:
            dut._token = None
        error.close()
        raise AmityHTTPError(error.code, "Firmware log download failed") from None
    after = identity()
    remaining()
    capture = LogCapture(data, dut.identity.device_id, before, after, expected_boot,
                         started, time.monotonic_ns())
    event("diagnostics.logs.captured", dut=dut.name, **capture.proof)
    return capture
