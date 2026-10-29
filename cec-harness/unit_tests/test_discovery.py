"""Host-only checks of generic DUT discovery and retained scan evidence."""

from pathlib import Path
from types import SimpleNamespace
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import CecAddress, PowerState
from amity_harness.discovery import ScanResult, scan_hdmi


def component(dut, *, name="A TV", gpio=22, logical=0, physical=0, power=PowerState.STANDBY):
    return SimpleNamespace(dut=dut, name=name, gpio=gpio, direction="front", device_type=0,
                           address=CecAddress(logical, physical), power=power)


def identity_row(device):
    return {"title": device.name, "bus": device.direction, "address": device.address.logical,
            "physical_address": device.address.physical, "type": device.device_type}


def exchanges(device, initiator=4, transport="wire"):
    records = []
    for index, (query, response) in enumerate(((0x46, 0x47), (0x83, 0x84))):
        common = {"component": device.name, "gpio": device.gpio, "transport": transport}
        timestamp = 11.0 + index
        records.extend([
            {**common, "direction": "rx", "source": initiator,
             "destination": device.address.logical, "opcode": query,
             "timestamp": timestamp, "wire_timestamp_ns": int(timestamp * 1e9)},
            {**common, "direction": "tx", "source": device.address.logical,
             "destination": initiator if response == 0x47 else 15, "opcode": response,
             "timestamp": timestamp + 0.1, "wire_timestamp_ns": int((timestamp + 0.1) * 1e9), "ok": True},
        ])
    return records


class Session:
    def __init__(self):
        self.components = []
        self.records = []
        self.healthy = True

    def assert_healthy(self):
        if not self.healthy:
            raise RuntimeError("injected unhealthy responder")


class Device:
    def __init__(self):
        self.name = "amity-dut-3"
        self.identity = SimpleNamespace(device_id="f" * 32)
        self.http_identity = self.identity.device_id
        self.before = {"discovering": False, "discovery_generation": 4, "expected_boot_id": "boot", "devices": []}
        self.after = []
        self.calls = []
        self.session = Session()
        self.trace = []

    def get_system(self):
        self.calls.append("get_system")
        return {"octopus_id": self.http_identity}

    def start_hdmi_scan(self):
        self.calls.append("start_hdmi_scan")
        self.session.records.extend(self.trace)
        return {"operation_id": "a" * 32, "stage": "complete"}

    def wait_for(self, predicate, **kwargs):
        self.calls.append("wait_for")
        states = self.after if "start_hdmi_scan" in self.calls else [self.before]
        for state in states:
            if predicate(state):
                return state
        raise TimeoutError("No scripted UI state met the scan condition")


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.dut = Device()
        self.tv = component(self.dut)
        self.dut.session.components = [self.tv]
        self.dut.trace = exchanges(self.tv)
        self.dut.after = [{"discovering": False, "discovery_generation": 5,
                           "expected_boot_id": "boot", "devices": [identity_row(self.tv)]}]

    def scan(self):
        with patch("amity_harness.discovery.time.monotonic_ns", return_value=10_000_000_000):
            return scan_hdmi(self.dut, self.dut.session)

    def test_arbitrary_named_dut_scans_only_its_powered_attached_components(self):
        off = component(self.dut, name="Off TV", gpio=25, power=PowerState.OFF)
        foreign = component(Device(), name="Other DUT TV", gpio=27)
        self.dut.session.components.extend([off, foreign])
        result = self.scan()
        self.assertIsInstance(result, ScanResult)
        self.assertIs(result, self.dut.last_scan)
        self.assertEqual(result.expected, (self.tv,))
        self.assertIs(result.assert_contains(self.tv), result)
        self.assertEqual(result.devices, [identity_row(self.tv)])
        self.assertEqual(result.initiators, {self.tv.gpio: 4})
        self.assertEqual(result.started, 10_000_000_000)
        self.assertEqual(self.dut.calls, ["get_system", "wait_for", "start_hdmi_scan", "wait_for"])
        for absent in (off, foreign):
            with self.assertRaises(AssertionError):
                result.assert_contains(absent)

    def test_failed_identity_is_raised_before_scan_and_keeps_partial_result(self):
        self.dut.http_identity = "0" * 32
        with self.assertRaisesRegex(AssertionError, "amity-dut-3"):
            self.scan()
        self.assertEqual(self.dut.calls, ["get_system"])
        self.assertIsInstance(self.dut.last_scan, ScanResult)
        self.assertEqual(self.dut.last_scan.started, 0)

    def test_stale_generation_cannot_pass_despite_matching_fresh_identities(self):
        self.dut.after[0]["discovery_generation"] = 4
        with self.assertRaises(TimeoutError):
            self.scan()
        self.assertEqual(self.dut.last_scan.status["discovery_generation"], 4)
        self.assertEqual(self.dut.last_scan.records, tuple(self.dut.trace))

    def test_changed_generation_and_matching_ui_cannot_replace_physical_evidence(self):
        self.dut.trace = exchanges(self.tv, transport="virtual")
        with self.assertRaises(TimeoutError):
            self.scan()
        result = self.dut.last_scan
        self.assertEqual(result.operation["stage"], "complete")
        self.assertEqual(result.devices, [identity_row(self.tv)])
        self.assertTrue(result.records)
        self.assertFalse(result.initiators)
        with self.assertRaises(TimeoutError):
            result.assert_contains(self.tv)

    def test_scan_waits_for_owner_to_finish_discovering(self):
        final = self.dut.after[0]
        self.dut.after = [{**final, "discovering": True}, final]
        result = self.scan()
        self.assertFalse(result.status["discovering"])
        result.assert_contains(self.tv)

    def test_dut_restart_is_raised_and_retains_final_observed_state(self):
        self.dut.after[0]["expected_boot_id"] = "restarted"
        with self.assertRaisesRegex(AssertionError, "restarted"):
            self.scan()
        self.assertEqual(self.dut.last_scan.status["expected_boot_id"], "restarted")
        self.assertTrue(self.dut.last_scan.records)

    def test_busy_to_idle_returns_empty_scan_then_explicit_assertion_requires_identity(self):
        self.dut.trace = []
        final = {**self.dut.after[0], "devices": []}
        self.dut.after = [{**final, "discovering": True}, final]
        result = self.scan()
        self.assertEqual(result.devices, [])
        with self.assertRaises(TimeoutError):
            result.assert_contains(self.tv)
        self.assertEqual(result.status, final)

    def test_scan_returns_partial_devices_without_implicitly_requiring_all_fixtures(self):
        absent = component(self.dut, name="Missing Peer", gpio=25)
        self.dut.session.components.append(absent)
        # A complete physical exchange for the responding TV allows the
        # missed-busy fallback, independently of the absent configured peer.
        result = self.scan()
        self.assertEqual(result.devices, [identity_row(self.tv)])
        result.assert_contains(self.tv)
        with self.assertRaises(TimeoutError):
            result.assert_contains(absent)

    def test_explicit_assertion_awaits_late_metadata_with_original_scan_evidence(self):
        final = {**self.dut.after[0], "devices": []}
        self.dut.after = [{**final, "discovering": True}, final]
        result = self.scan()
        self.assertFalse(result.devices)
        self.dut.after = [{**final, "devices": [identity_row(self.tv)]}]
        self.assertIs(result.assert_contains(self.tv), result)
        self.assertEqual(result.devices, [identity_row(self.tv)])
        self.assertEqual(result.records, tuple(self.dut.trace))
        self.assertEqual(self.dut.calls.count("start_hdmi_scan"), 1)

    def test_assertion_does_not_renew_expired_scan_deadline(self):
        final = {**self.dut.after[0], "devices": []}
        self.dut.after = [{**final, "discovering": True}, final]
        result = self.scan()
        calls_before = list(self.dut.calls)
        with patch("amity_harness.discovery.time.monotonic", return_value=result._deadline + 1):
            with self.assertRaises(TimeoutError):
                result.assert_contains(self.tv)
        self.assertEqual(self.dut.calls, calls_before)

    def test_assertion_cannot_poll_after_session_has_ended(self):
        final = {**self.dut.after[0], "devices": []}
        self.dut.after = [{**final, "discovering": True}, final]
        result = self.scan()
        self.dut.session._stop = threading.Event()
        self.dut.session._stop.set()
        calls_before = list(self.dut.calls)
        with self.assertRaisesRegex(AssertionError, "session ended"):
            result.assert_contains(self.tv)
        self.assertEqual(self.dut.calls, calls_before)

    def test_submission_acknowledgement_alone_cannot_complete_even_an_empty_fixture(self):
        self.dut.session.components.clear()
        self.dut.trace = []
        self.dut.after[0]["devices"] = []
        with self.assertRaises(TimeoutError):
            self.scan()

    def test_missed_busy_fallback_needs_full_physical_identity_exchange(self):
        self.dut.trace = self.dut.trace[:2]  # OSD exchange only, no physical-address exchange.
        with self.assertRaises(TimeoutError):
            self.scan()

    def test_invalid_timeout_cannot_start_a_scan(self):
        for timeout in (0, -1, True, float("nan"), float("inf")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                scan_hdmi(self.dut, self.dut.session, timeout=timeout)
        self.assertFalse(self.dut.calls)


if __name__ == "__main__":
    unittest.main()
