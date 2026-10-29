"""Host-only framework checks; these do not run a functional DUT scenario."""

import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.dut import AmityHTTPError, DUT, PinctrlGPIO
from amity_harness.wiring import Wiring


DOCUMENT = """# Test Harness
## DUTs
| Bench label | ESP32-S3 MAC / USB serial | Amity device ID |
|---|---|---|
| amity-dut | 68:EE:8F:50:2B:44 | dfcddb1bb98d70eca83cd015db66a2ad |
## Setup/reset GPIO wiring
| Bench label | DUT GPIO (XIAO pin) | Harness GPIO (BCM/RP1) | Pi physical header pin |
|---|---|---:|---:|
| amity-dut | 4 (D3) | 4 | 7 |
## HDMI-CEC harness GPIO mapping
| Device | Device Tentacle # | Tentacle Front/Back | DUT GPIO | Harness GPIO |
|---|---:|---|---:|---:|
| amity-dut | 0 | front | 1 | 23 |
| amity-dut | 0 | back | 2 | 24 |
"""


class Opener:
    def __init__(self, *responses):
        self.responses = list(responses)
        self.requests = []

    def open(self, request, timeout):
        self.requests.append(request)
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return io.BytesIO(json.dumps(response).encode())


class WiringTests(unittest.TestCase):
    def parse(self, document=DOCUMENT):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "TEST-HARNESS.md"
            source.write_text(document)
            return Wiring.from_markdown(source)

    def test_named_gpio_lookup(self):
        wiring = self.parse()
        self.assertEqual(wiring.cec_gpio("amity-dut", 0, "front"), 23)
        self.assertEqual(wiring.cec_gpio("amity-dut", 0, "back"), 24)
        self.assertEqual(wiring.reset_gpio("amity-dut"), 4)
        self.assertEqual(wiring.identities["amity-dut"].mac, "68:EE:8F:50:2B:44")
        with self.assertRaises(ValueError):
            wiring.cec_gpio("amity-dut", 1, "front")
        with self.assertRaises(ValueError):
            DUT("unknown", wiring=wiring)

    def test_markdown_is_the_source_of_truth(self):
        changed = DOCUMENT.replace("| 1 | 23 |", "| 1 | 22 |")
        self.assertEqual(self.parse(changed).cec_gpio("amity-dut", 0, "front"), 22)

    def test_bad_wiring_is_rejected_before_hardware_access(self):
        corruptions = [
            DOCUMENT.replace("| 2 | 24 |", "| 2 | 23 |"),
            DOCUMENT.replace("| 2 | 24 |", "| 2 | 4 |"),
            DOCUMENT.replace("| back |", "| backwards |"),
            DOCUMENT.replace("| 2 | 24 |", "| 2 | invalid |"),
            DOCUMENT + "\n## HDMI-CEC harness GPIO mapping\n",
            DOCUMENT.replace("| amity-dut | 0 | back | 2 | 24 |\n", ""),
            DOCUMENT.replace("| amity-dut | 0 | back", "| another-dut | 0 | back"),
        ]
        for document in corruptions:
            with self.subTest(document=document), self.assertRaises(ValueError):
                self.parse(document)


class DUTTests(unittest.TestCase):
    def setUp(self):
        self.wiring = WiringTests().parse()

    def test_ui_login_state_and_scan_use_existing_routes(self):
        opener = Opener({"token": "session"}, {"ready": True}, {"stage": "complete"})
        dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                  password="private password", opener=opener)
        self.assertTrue(dut.get_state()["ready"])
        dut.start_hdmi_scan()
        self.assertEqual([request.full_url for request in opener.requests], [
            "https://amity.local/api/login",
            "https://amity.local/api/v2/activity-editor/status",
            "https://amity.local/api/v2/activity-editor/discover",
        ])
        self.assertEqual(opener.requests[1].get_header("Authorization"), "Bearer session")
        scan = json.loads(opener.requests[2].data)
        self.assertEqual(set(scan), {"request_id"})
        self.assertEqual(len(scan["request_id"]), 32)

    def test_request_requires_login(self):
        opener = Opener()
        dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local", opener=opener)
        with self.assertRaises(RuntimeError):
            dut.get_state()
        self.assertFalse(opener.requests)

    def test_name_supplies_origin_without_network_until_login(self):
        opener = Opener({"token": "session"}, {"octopus_id": self.wiring.identities["amity-dut"].device_id})
        dut = DUT("amity-dut", wiring=self.wiring, opener=opener)
        self.assertEqual(dut.url, "https://amity-dut.local")
        self.assertFalse(opener.requests)
        dut.login("secret")
        self.assertEqual(dut.get_system()["octopus_id"], dut.identity.device_id)
        self.assertEqual(opener.requests[-1].full_url, "https://amity-dut.local/api/v2/system")

    def test_invalid_polling_durations_are_rejected_before_requests(self):
        opener = Opener()
        dut = DUT("amity-dut", wiring=self.wiring, opener=opener)
        for value in (float("nan"), float("inf"), 0, -1, True):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    dut.wait_for(lambda state: True, timeout=value)
                with self.assertRaises(ValueError):
                    dut.wait_for(lambda state: True, poll_interval=value)
                with self.assertRaises(ValueError):
                    dut.wait_operation({"stage": "complete"}, timeout=value)
                with self.assertRaises(ValueError):
                    dut.wait_operation({"stage": "complete"}, poll_interval=value)
                with self.assertRaises(ValueError):
                    dut.set_configuration({}, timeout=value)
        self.assertFalse(opener.requests)

    def test_expired_session_is_not_reused(self):
        error = HTTPError("https://amity.local/api/v2/activity-editor/status", 401,
                          "Unauthorized", {}, io.BytesIO(b'{"error":"Login required"}'))
        opener = Opener({"token": "session"}, error)
        dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                  password="secret", opener=opener)
        with self.assertRaises(AmityHTTPError) as failure:
            dut.get_state()
        self.assertEqual(failure.exception.status, 401)
        with self.assertRaises(RuntimeError):
            dut.get_state()

    def test_configuration_retains_ui_revision_checks(self):
        opener = Opener({"token": "session"}, {"operation_id": "a" * 32, "stage": "configuring"},
                        {"operation_id": "a" * 32, "stage": "applied"})
        dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                  password="secret", opener=opener)
        configuration = {"version": 1, "expected_config_revision": "12",
                         "expected_presentation_revision": "4", "activities": [],
                         "device_choices": [{"title": "TV"}], "quirks": []}
        with patch("amity_harness.dut.time.sleep"):
            result = dut.set_configuration(configuration)
        self.assertEqual(result["stage"], "applied")
        payload = json.loads(opener.requests[1].data)
        self.assertEqual(payload["expected_config_revision"], "12")
        self.assertEqual(payload["expected_presentation_revision"], "4")
        self.assertNotIn("device_choices", payload)
        self.assertIn("device_choices", configuration)

    def test_reset_always_releases_gpio_including_cancelled_hold(self):
        class GPIO:
            def __init__(self):
                self.calls = []

            def low(self, pin):
                self.calls.append(("low", pin))

            def release(self, pin):
                self.calls.append(("release", pin))

        gpio = GPIO()
        dut = DUT("amity-dut", wiring=self.wiring, gpio=gpio)
        with patch("amity_harness.dut.time.sleep", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            dut.press_fw_reset(3.5)
        self.assertEqual(gpio.calls, [("low", 4), ("release", 4)])
        gpio.calls.clear()
        for seconds in (-1, 0, float("nan"), float("inf"), True):
            with self.assertRaises(ValueError):
                dut.press_fw_reset(seconds)
        self.assertFalse(gpio.calls)

    def test_pinctrl_release_is_high_impedance_without_pull(self):
        calls = []
        gpio = PinctrlGPIO(runner=lambda command, **kwargs: calls.append(command))
        gpio.low(4)
        gpio.release(4)
        self.assertEqual(calls, [["pinctrl", "set", "4", "dl"],
                                 ["pinctrl", "set", "4", "op"],
                                 ["pinctrl", "set", "4", "ip", "pn"]])

    def test_credentials_are_read_only_from_private_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credentials.json"
            path.write_text('{"admin_password":"secret"}')
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                DUT("amity-dut", wiring=self.wiring, credentials=path)
            path.chmod(0o600)
            opener = Opener({"token": "session"})
            DUT("amity-dut", wiring=self.wiring, url="https://amity.local", credentials=path, opener=opener)
            self.assertEqual(json.loads(opener.requests[0].data), {"password": "secret"})


if __name__ == "__main__":
    unittest.main()
