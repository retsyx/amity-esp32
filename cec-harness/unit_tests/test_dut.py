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
from amity_harness.wiring import DEFAULT_WIRING, Wiring
from unit_tests.harness_pins import cec_gpio, reset_gpio, wiring


def single_dut_document():
    """Keep one board from the actual Markdown, preserving every pin value."""
    names = set(wiring().identities)
    lines = []
    for line in DEFAULT_WIRING.read_text().splitlines():
        if line.lstrip().startswith("|"):
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            if cells[0] in names and cells[0] != "amity-dut":
                continue
            line = "| " + " | ".join(cells) + " |"
        lines.append(line)
    return "\n".join(lines) + "\n"


DOCUMENT = single_dut_document()


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
        self.assertEqual(wiring.cec_gpio("amity-dut", 0, "front"), cec_gpio(direction="front"))
        self.assertEqual(wiring.cec_gpio("amity-dut", 0, "back"), cec_gpio())
        self.assertEqual(wiring.reset_gpio("amity-dut"), reset_gpio())
        self.assertEqual(wiring.identities["amity-dut"].mac, "68:EE:8F:50:2B:44")
        with self.assertRaises(ValueError):
            wiring.cec_gpio("amity-dut", 1, "front")
        with self.assertRaises(ValueError):
            DUT("unknown", wiring=wiring)

    def test_markdown_is_the_source_of_truth(self):
        original, replacement = cec_gpio(direction="front"), cec_gpio("amity-dut-2", direction="front")
        changed = DOCUMENT.replace(f"| {original} |", f"| {replacement} |")
        self.assertEqual(self.parse(changed).cec_gpio("amity-dut", 0, "front"), replacement)

    def test_bad_wiring_is_rejected_before_hardware_access(self):
        corruptions = [
            DOCUMENT.replace(f"| {cec_gpio()} |", f"| {cec_gpio(direction='front')} |"),
            DOCUMENT.replace(f"| {cec_gpio()} |", f"| {reset_gpio()} |"),
            DOCUMENT.replace("| back |", "| backwards |"),
            DOCUMENT.replace(f"| {cec_gpio()} |", "| invalid |"),
            DOCUMENT + "\n## HDMI-CEC harness GPIO mapping\n",
            "\n".join(line for line in DOCUMENT.splitlines() if not line.startswith("| amity-dut | 0 | back |")),
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
            "https://amity.local/api/activity-editor/status",
            "https://amity.local/api/activity-editor/discover",
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
        self.assertEqual(opener.requests[-1].full_url, "https://amity-dut.local/api/system")

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
        error = HTTPError("https://amity.local/api/activity-editor/status", 401,
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
                         "device_choices": [{"title": "TV"}]}
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
        pin = self.wiring.reset_gpio(dut.name)
        self.assertEqual(gpio.calls, [("low", pin), ("release", pin)])
        gpio.calls.clear()
        for seconds in (-1, 0, float("nan"), float("inf"), True):
            with self.assertRaises(ValueError):
                dut.press_fw_reset(seconds)
        self.assertFalse(gpio.calls)

    def test_pinctrl_release_is_high_impedance_without_pull(self):
        calls = []
        gpio = PinctrlGPIO(runner=lambda command, **kwargs: calls.append(command))
        pin = self.wiring.reset_gpio("amity-dut")
        gpio.low(pin)
        gpio.release(pin)
        self.assertEqual(calls, [["pinctrl", "set", str(pin), "dl"],
                                 ["pinctrl", "set", str(pin), "op"],
                                 ["pinctrl", "set", str(pin), "ip", "pn"]])

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
