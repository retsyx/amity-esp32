"""Host-only framework checks; these do not run a functional DUT scenario."""

import io
import json
import ssl
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.dut import AmityHTTPError, DUT, PinctrlGPIO
from amity_harness.config import RunConfig, use_config
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


def administration_document(hostname):
    """Add the optional inventory column without duplicating bench wiring."""
    before, after = DOCUMENT.split("## Setup/reset GPIO wiring", 1)
    lines = []
    for line in before.splitlines():
        if line.startswith("|"):
            cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
            value = ("Administration hostname" if cells[0] == "Bench label"
                     else "---" if cells[0] == "---" else hostname)
            line = "| " + " | ".join((*cells, value)) + " |"
        lines.append(line)
    return "\n".join(lines) + "\n## Setup/reset GPIO wiring" + after


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
        self.assertEqual(wiring.dut_cec_gpio("amity-dut", 0, "front"), 1)
        self.assertEqual(wiring.dut_cec_gpio("amity-dut", 0, "back"), 2)
        self.assertEqual(wiring.identities["amity-dut"].mac, "68:EE:8F:50:2B:44")
        with self.assertRaises(ValueError):
            wiring.cec_gpio("amity-dut", 1, "front")
        with self.assertRaises(ValueError):
            DUT("unknown", wiring=wiring)

    def test_markdown_is_the_source_of_truth(self):
        original, replacement = cec_gpio(direction="front"), cec_gpio("amity-dut-2", direction="front")
        changed = DOCUMENT.replace(f"| {original} |", f"| {replacement} |")
        self.assertEqual(self.parse(changed).cec_gpio("amity-dut", 0, "front"), replacement)

    def test_optional_administration_hostname(self):
        configured = self.parse(administration_document("bench-main"))
        self.assertEqual(configured.identities["amity-dut"].hostname, "bench-main")
        self.assertEqual(DUT("amity-dut", wiring=configured).url, "https://bench-main.local")
        self.assertIsNone(wiring().identities["amity-dut-2"].hostname)
        for document in (DOCUMENT, administration_document("")):
            parsed = self.parse(document)
            self.assertIsNone(parsed.identities["amity-dut"].hostname)
            self.assertEqual(DUT("amity-dut", wiring=parsed).url, "https://amity-dut.local")

    def test_hostname_validation_matches_factory_names(self):
        for hostname in ("a", "bench-tv", "A" * 32):
            with self.subTest(hostname=hostname):
                parsed = self.parse(administration_document(hostname))
                self.assertEqual(parsed.identities["amity-dut"].hostname, hostname.lower())
        for hostname in ("-amity", "amity-", "amity.local", "https://amity", "amity host", "amity_host",
                         "amity*host", "a" * 33, "amité"):
            with self.subTest(hostname=hostname), self.assertRaises(ValueError):
                self.parse(administration_document(hostname))

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

    def test_default_transport_keeps_selected_TLS_verification_context(self):
        dut = DUT("amity-dut", wiring=self.wiring)
        self.assertTrue(dut._opener._context.check_hostname)
        self.assertEqual(dut._opener._context.verify_mode, ssl.CERT_REQUIRED)
        other = DUT("amity-dut", wiring=self.wiring, insecure=True)
        self.assertFalse(other._opener._context.check_hostname)
        self.assertEqual(other._opener._context.verify_mode, ssl.CERT_NONE)
        self.assertIsNot(dut._opener, other._opener)
        self.assertIsNot(dut._opener._context, other._opener._context)
        self.assertIsNone(dut._opener._connection)
        dut.close();other.close()

    def test_login_failure_and_logout_failure_close_transport(self):
        class ClosingOpener(Opener):
            closed = 0
            def close(self):
                self.closed += 1
        error = lambda: HTTPError("https://amity.local/api/login", 401, "Unauthorized", {},
                                  io.BytesIO(b'{"error":"Login required"}'))
        opener = ClosingOpener(error())
        dut = DUT("amity-dut", wiring=self.wiring, opener=opener)
        with self.assertRaises(AmityHTTPError):
            dut.login("secret")
        self.assertIsNone(dut._token)
        self.assertEqual(opener.closed, 2)  # New login boundary and its failure.
        opener = ClosingOpener({"token": "session"}, error())
        dut = DUT("amity-dut", wiring=self.wiring, opener=opener)
        dut.login("secret")
        with self.assertRaises(AmityHTTPError):
            dut.logout()
        self.assertIsNone(dut._token)
        self.assertEqual(opener.closed, 2)

    def test_documented_hostname_supplies_origin_without_network_until_login(self):
        for document, origin in ((DOCUMENT, "https://amity-dut.local"),
                                 (administration_document("bench-main"), "https://bench-main.local")):
            with self.subTest(origin=origin):
                parsed = WiringTests().parse(document)
                opener = Opener({"token": "session"}, {"octopus_id": parsed.identities["amity-dut"].device_id})
                dut = DUT("amity-dut", wiring=parsed, opener=opener)
                self.assertEqual(dut.url, origin)
                self.assertFalse(opener.requests)
                dut.login("secret")
                self.assertEqual(dut.get_system()["octopus_id"], dut.identity.device_id)
                self.assertEqual(opener.requests[-1].full_url, origin + "/api/system")

    def test_url_overrides_take_precedence_over_documented_hostname(self):
        documented = WiringTests().parse(administration_document("bench-main"))
        config = RunConfig(default_url="https://default.example", urls={"amity-dut": "https://main.example"})
        with use_config(config):
            self.assertEqual(DUT("amity-dut", wiring=documented).url, "https://main.example")
            self.assertEqual(DUT("amity-dut-2").url, "https://default.example")
            self.assertEqual(DUT("amity-dut", wiring=documented, url="https://explicit.example").url,
                             "https://explicit.example")
        self.assertEqual(DUT("amity-dut-2").url, "https://amity-dut-2.local")

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
        self.assertTrue(error.closed)
        with self.assertRaises(RuntimeError):
            dut.get_state()

    def test_configuration_retains_ui_revision_checks(self):
        opener = Opener({"token": "session"}, {"operation_id": "a" * 32, "stage": "configuring"},
                        {"operation_id": "a" * 32, "stage": "applied"})
        dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                  password="secret", opener=opener)
        row = {"activity_id": "6" * 32, "name": "Watch", "display": "native:tv", "source": "",
               "audio": "", "switch": None, "status": "read-only"}
        configuration = {"version": 1, "expected_config_revision": "12",
                         "expected_presentation_revision": "4", "activities": [row],
                         "device_choices": [{"title": "TV"}], "component_owners": [{"owner": "main"}]}
        with patch("amity_harness.dut.time.sleep"):
            result = dut.set_configuration(configuration)
        self.assertEqual(result["stage"], "applied")
        payload = json.loads(opener.requests[1].data)
        # Exactly the UI's submission fields, as accepted by the firmware.
        self.assertEqual(set(payload), {"version", "request_id", "expected_config_revision",
                                        "expected_presentation_revision", "activities"})
        self.assertEqual(payload["expected_config_revision"], "12")
        self.assertEqual(payload["expected_presentation_revision"], "4")
        self.assertEqual(payload["activities"], [{key: row[key] for key in row if key != "status"}])
        self.assertIn("device_choices", configuration)

    def test_operation_success_requires_a_confirmed_ui_stage(self):
        for stage in (None, "", "done", "scheduled"):
            with self.subTest(stage=stage):
                opener = Opener({"token": "session"})
                dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                          password="secret", opener=opener)
                operation = {"operation_id": "a" * 32}
                if stage is not None:
                    operation["stage"] = stage
                with self.assertRaisesRegex(RuntimeError, "could not confirm"):
                    dut.wait_operation(operation)
        for stage in ("complete", "applied"):
            with self.subTest(stage=stage):
                dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                          password="secret", opener=Opener({"token": "session"}))
                self.assertEqual(dut.wait_operation({"operation_id": "a" * 32, "stage": stage})["stage"], stage)

    def test_http_error_preserves_typed_code_without_logging_response_or_credentials(self):
        for code in ("stale_removal", None, {"unexpected": "PRIVATE_BODY"}):
            with self.subTest(code_type=type(code).__name__):
                payload = {"code": code, "error": "Activities changed", "password": "PRIVATE_BODY"}
                error = HTTPError("https://amity.local/api/membership/remove", 409, "Conflict", {},
                                  io.BytesIO(json.dumps(payload).encode()))
                opener = Opener({"token": "PRIVATE_BEARER"}, error)
                with patch("amity_harness.dut.event") as events:
                    dut = DUT("amity-dut", wiring=self.wiring, url="https://amity.local",
                              password="PRIVATE_PASSWORD", opener=opener)
                    with self.assertRaises(AmityHTTPError) as failure:
                        dut.request("/api/membership/remove", {"request_id": "1" * 32})
                self.assertEqual(failure.exception.status, 409)
                self.assertEqual(failure.exception.code, code if isinstance(code, str) else None)
                self.assertTrue(error.closed)
                self.assertEqual(len(opener.requests), 2)
                self.assertNotIn("PRIVATE_", str(events.call_args_list))

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
