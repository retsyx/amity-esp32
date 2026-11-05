"""A native-USB reboot preserves the original main's saved configuration."""

import http.client
import time
from urllib.error import HTTPError, URLError

from amity_harness import DUT
from amity_harness.discovery import _identifier
from amity_harness.dut import AmityHTTPError
from amity_harness.enrollment import _authenticated_system, _unreachable
from amity_harness.firmware import saved_configuration
from amity_harness.trace import event


def test_usb_reboot():
    dut = DUT("amity-dut")
    try:
        dut.authenticate()
        before = dut.get_system()
        if (before.get("octopus_id") != dut.identity.device_id
                or not _identifier(before.get("boot_id")) or before.get("ready") is not True):
            raise AssertionError("The original main must be ready before its USB reboot")
        configuration = saved_configuration(dut.request("/api/config"))
        password = DUT._read_password(dut._runtime.credentials_for(dut.name))
        dut.logout()
        dut.usb_reboot()
        deadline = time.monotonic() + dut._runtime.timeout

        def remaining():
            budget = deadline - time.monotonic()
            if budget <= 0:
                raise TimeoutError("USB reboot did not produce a fresh, ready main")
            return budget

        while True:
            try:
                ready = dut.probe_ready(timeout=min(3, remaining()))
                if (ready.get("device_id") != dut.identity.device_id
                        or not _identifier(ready.get("boot_id"))):
                    raise AssertionError("USB reboot readiness returned another or invalid DUT identity")
                if ready.get("ready") is True and ready["boot_id"] != before["boot_id"]:
                    system = _authenticated_system(dut, password, min(15, remaining()))
                    if (system.get("boot_id") != ready["boot_id"] or any(
                            system.get(key) != before.get(key)
                            for key in ("consortium_id", "mode", "hostname"))):
                        raise AssertionError("USB reboot changed the original main's identity or configuration")
                    if system.get("ready") is True and system.get("configuration_pending") is False:
                        config = dut.request("/api/config", timeout=min(15, remaining()))
                        if saved_configuration(config) != configuration:
                            raise AssertionError("USB reboot changed the saved configuration")
                        if (system.get("applied_config_revision") == config.get("config_revision")
                                and system.get("applied_presentation_revision") == config.get("presentation_revision")):
                            event("usb.reboot.verified", dut=dut.name, device_id=dut.identity.device_id,
                                  old_boot=before["boot_id"], new_boot=system["boot_id"],
                                  configuration_unchanged=True)
                            return
            except (OSError, URLError, http.client.HTTPException, AmityHTTPError) as error:
                if isinstance(error, (HTTPError, AmityHTTPError)):
                    status = error.code if isinstance(error, HTTPError) else error.status
                    if status not in {408, 429, 500, 502, 503, 504}:
                        raise
                elif not _unreachable(error):
                    raise
            time.sleep(min(0.25, remaining()))
    finally:
        dut.logout()
