"""Real-keyboard fixture ownership and shipping UI pairing fences."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import json
import io
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
from contextlib import redirect_stdout, redirect_stderr

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import CecError, CecSession, DUT
from amity_harness.ble_keyboard import KeyboardConflict, LocalKeyboard, PairedKeyboard, PairingFailed
from amity_harness.ble_keyboard_worker import LINK_DROP_TIMEOUT
from amity_harness.config import RunConfig
from amity_harness.dut import AmityHTTPError
from amity_harness.runner import main as run_main
from unit_tests.virtual_time import use_virtual_time


class Fixture:
    def __init__(self):
        self.main = DUT("amity-dut")
        self.boot, self.group = "a" * 32, "b" * 32
        self.calls, self.worker_calls, self.cleanups = [], [], []
        self.system = {"octopus_id": self.main.identity.device_id, "boot_id": self.boot,
                       "consortium_id": self.group, "mode": "coordinator",
                       "membership": {"coordinator": True, "member": False, "authority_ready": True}}
        self.state = {"ready": True, "boot_id": self.boot, "remote_generation": "0",
                      "scanning": False, "bonded": False, "connected": False, "pairing": False,
                      "connecting": False, "unpairing": False, "too_far": False, "last_error": 0}
        self.activity = {"expected_boot_id": self.boot, "current_activity": 0, "activity_settling": False,
                         "expected_config_revision": "4", "expected_presentation_revision": "4"}
        self.session = SimpleNamespace(assert_healthy=lambda: None)
        self.bed = SimpleNamespace(_entered=True, _closed=False, duts=(self.main,), members=(),
                                   control_only=(), session=self.session, add_cleanup=self.cleanups.append)
        self.main._testbed = self.bed
        self.worker = Worker(self)
        self.main._runtime = RunConfig(timeout=30, keyboard_factory=lambda: self.worker)
        self.main.get_system = lambda: deepcopy(self.system)
        self.main.get_state = lambda: deepcopy(self.activity)
        self.main.request = self.request
        self.main.last_scan = SimpleNamespace(dut=self.main, _session=self.session, initiators={23: 4},
                                             owner_boots={self.main.identity.device_id: self.boot},
                                             owner_duts={self.main.identity.device_id: self.main},
                                             expected=(), validate_owners=lambda expected, **kwargs: None)
        self.lose_pair_reply = self.lose_unpair_reply = self.reject_pair = False
        # Untargeted pairing: Amity searches, then connects to the first
        # eligible advertiser. Each status read advances that search once.
        self.search_reads = 0
        self.too_far = self.other_remote = False
        self.pairing_statuses = []
        self.terminal_pair_error = None
        self.drop_receipt = None
        self.reconnect_after_drops = True

    def request(self, path, data=None, **kwargs):
        self.calls.append((path, deepcopy(data)))
        if path == "/api/remotes" and data is None:
            if self.state["pairing"]:
                self.pairing_statuses.append(deepcopy(self.state))
                if self.search_reads:
                    self.search_reads -= 1
                    self.state["too_far"] = self.too_far
                else:
                    self.finish_search()
            return deepcopy(self.state)
        if path not in ("/api/remotes/pair", "/api/remotes/unpair"):
            raise AssertionError("Unexpected DUT route: " + path)
        self.assert_payload(path, data)
        if path.endswith("/pair"):
            assert not any(self.state[name] for name in ("scanning", "pairing", "connecting", "bonded")), \
                "Pair starts only from an idle, unbonded remote"
        if self.reject_pair and path.endswith("/pair"):
            raise AmityHTTPError(409, "Changed pairing review")
        self.state["remote_generation"] = str(int(self.state["remote_generation"]) + 1)
        paired = path.endswith("/pair")
        if paired:
            self.state.update(pairing=True, scanning=True, last_error=0)
            if not self.search_reads:
                self.finish_search()
        else:
            self.state.update(bonded=False, connected=False, scanning=False)
            self.worker.connected = self.worker.ready = False
        if paired and self.lose_pair_reply:
            self.lose_pair_reply = False
            raise OSError("Pair reply lost")
        if not paired and self.lose_unpair_reply:
            self.lose_unpair_reply = False
            raise OSError("Unpair reply lost")
        return {"saved": True, "admitted": True, "duplicate": False,
                "remote_generation": self.state["remote_generation"]}

    def finish_search(self):
        self.state.update(pairing=False, scanning=False, too_far=False)
        if self.terminal_pair_error is not None:
            # The Pi side may complete Just Works before the DUT rejects it.
            self.state.update(bonded=False, connected=False, connecting=False,
                              unpairing=False, last_error=self.terminal_pair_error, too_far=self.too_far)
            self.worker.bonded = True
            return
        self.state.update(bonded=True, connected=True)
        if not self.other_remote:
            self.worker.connected = self.worker.ready = self.worker.bonded = True

    def assert_payload(self, path, data):
        # The UI's exact approval, identical for Pair and Unpair.
        assert data["expected_boot_id"] == self.boot
        assert data["expected_remote_generation"] == self.state["remote_generation"]
        assert len(data["request_id"]) == 32
        assert set(data) == {"request_id", "expected_boot_id", "expected_remote_generation"}


def link_drop(number, **changes):
    return {"number": number, "handle": 64 + number, "reaction_ns": 400_000, "link_ns": 30_000_000,
            "command_status": 0, "reason": 0x13, "encrypted": False, **changes}


class Worker:
    def __init__(self, fixture):
        self.fixture = fixture
        self.connected = self.bonded = self.ready = False
        self.advertisement_registered = True
        self.peer_owned = True

    def open(self):
        self.fixture.worker_calls.append(("open",))
        return {"address": "12:34:56:78:9a:bc", "type": 0, "name": "Amity Test Keyboard", "protocol_version": 1}

    def status(self):
        return {"ready": self.ready, "connected": self.connected, "bonded": self.bonded,
                "peer_owned": self.peer_owned,
                "advertisement_registered": self.advertisement_registered}

    def forget_peer(self):
        assert not self.connected
        self.fixture.worker_calls.append(("forget_peer",))
        self.bonded = self.ready = False
        return {"cleared": True}

    def report(self, *, keyboard=(), consumer=0):
        self.fixture.worker_calls.append(("report", tuple(keyboard), consumer))
        assert self.connected and self.ready
        return {"timestamp_monotonic_ns": time.monotonic_ns(), "keyboard_report": "0000000000000000",
                "consumer_report": "0000", "changed": [1, 2]}

    def disconnect(self):
        self.fixture.worker_calls.append(("disconnect",))
        self.connected = self.ready = False
        self.fixture.state["connected"] = False
        return {"connected": False, "disconnected_monotonic_ns": time.monotonic_ns()}

    def drop_links(self, count):
        self.fixture.worker_calls.append(("drop_links", count))
        self.connected = self.ready = self.fixture.reconnect_after_drops
        self.fixture.state["connected"] = self.fixture.reconnect_after_drops
        if self.fixture.drop_receipt is not None:
            return deepcopy(self.fixture.drop_receipt)
        return {"disconnected_monotonic_ns": time.monotonic_ns(), "completed": True,
                "drops": [link_drop(number) for number in range(1, count + 1)]}

    def close(self):
        self.fixture.worker_calls.append(("close",))
        self.connected = self.ready = self.bonded = False


class PairedKeyboardTests(unittest.TestCase):
    def setUp(self):
        self.clock = use_virtual_time(self)

    def test_untargeted_pair_and_owned_cleanup_use_fresh_shipping_ui_reviews(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            self.assertIs(keyboard.assert_paired(), keyboard)
            action = keyboard.report(keyboard=(0x4f,), consumer=0xe9)
            self.assertEqual(action.kind, "ble_report")
            self.assertEqual(action.boot, fixture.boot)
            self.assertLessEqual(action.started_ns, action.operation["timestamp_monotonic_ns"])
            self.assertEqual(action.owner_boots, fixture.main.last_scan.owner_boots)
        paths = [path for path, _ in fixture.calls]
        self.assertEqual(paths.count("/api/remotes/pair"), 1)
        self.assertEqual(paths.count("/api/remotes/unpair"), 1)
        self.assertFalse(fixture.state["bonded"])
        self.assertEqual(fixture.state["remote_generation"], "2")
        phases = [row["phase"] for row in fixture.bed.control_observations[0]["checks"]]
        self.assertEqual(phases, ["pair_submit", "paired", "report", "unpair_submit", "unpaired"])

    def test_explicit_wide_keyboard_capacity_reaches_factory_and_report_context(self):
        fixture = Fixture()
        slots_seen = []
        def factory(*, keyboard_slots):
            slots_seen.append(keyboard_slots)
            return fixture.worker
        fixture.main._runtime = RunConfig(timeout=30, keyboard_factory=factory)
        original_open = fixture.worker.open
        fixture.worker.open = lambda: {**original_open(), "keyboard_slots": 10}
        original_report = fixture.worker.report
        def report(**kwargs):
            return {**original_report(**kwargs), "keyboard_report": "00" * 12}
        fixture.worker.report = report
        usages = (0x28, 0x29, 0x4a, 0x4f, 0x50, 0x51, 0x52, 0x7f, 0x80, 0x81)
        with fixture.main.pair_ble_keyboard(keyboard_slots=10) as keyboard:
            with keyboard.reports() as reports:
                action = reports.report(keyboard=usages)
                self.assertEqual(len(action.operation["keyboard_report"]), 24)
        self.assertEqual(slots_seen, [10])
        self.assertIn(("report", usages, 0), fixture.worker_calls)
        self.assertIn("--keyboard-slots", LocalKeyboard(keyboard_slots=10)._command())
        self.assertEqual(LocalKeyboard(keyboard_slots=10)._command()[-1], "10")
        with self.assertRaises(ValueError): LocalKeyboard(keyboard_slots=7)

    def test_one_advertisement_pairs_again_after_its_owned_unpair(self):
        fixture = Fixture()
        with PairedKeyboard(fixture.main) as keyboard:
            keyboard.open()
            for number in (1, 2):
                keyboard.pair().assert_paired()
                self.assertEqual(fixture.worker_calls.count(("open",)), 1)
                self.assertEqual(fixture.worker_calls.count(("close",)), 0)
                if number == 1:
                    keyboard.unpair()
                    self.assertEqual(fixture.worker_calls.count(("forget_peer",)), 1)
                    with self.assertRaisesRegex(RuntimeError, "Open the BLE keyboard|Unpair"):
                        keyboard.unpair()
        mutations = [path for path, data in fixture.calls if data is not None]
        self.assertEqual(mutations, ["/api/remotes/pair", "/api/remotes/unpair"] * 2)
        self.assertEqual(fixture.worker_calls.count(("close",)), 1)
        self.assertEqual(fixture.state["remote_generation"], "4")

    def test_pair_requires_a_live_unconnected_advertisement_and_an_idle_remote(self):
        for changed in ("advertisement", "peer", "searching", "repeated"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                with PairedKeyboard(fixture.main) as keyboard:
                    keyboard.open()
                    if changed == "advertisement": fixture.worker.advertisement_registered = False
                    if changed == "peer": fixture.worker.connected = True
                    if changed == "searching": fixture.state["scanning"] = True
                    if changed == "repeated":
                        keyboard.pair()
                        with self.assertRaisesRegex(RuntimeError, "Unpair this keyboard"): keyboard.pair()
                        continue
                    with self.assertRaises(KeyboardConflict): keyboard.pair()
                    fixture.state["scanning"] = False
                paths = [path for path, data in fixture.calls if data is not None]
                self.assertEqual(paths.count("/api/remotes/pair"), int(changed == "repeated"))

    def test_unpair_error_or_foreign_generation_does_not_clear_pi_peer_or_start_new_scan(self):
        for changed in ("error", "generation"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                keyboard = fixture.main.pair_ble_keyboard()
                if changed == "error":
                    original = fixture.main.request
                    def request(path, data=None, **kwargs):
                        result = original(path, data, **kwargs)
                        if path == "/api/remotes/unpair": fixture.state["last_error"] = 77
                        return result
                    fixture.main.request = request
                else:
                    fixture.state.update(remote_generation="99", bonded=False, connected=False)
                with self.assertRaises(KeyboardConflict): keyboard.unpair()
                self.assertNotIn(("forget_peer",), fixture.worker_calls)
                with self.assertRaises(KeyboardConflict): keyboard.close()
                self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_pi_peer_clear_failure_is_explicit_and_final_cleanup_still_closes_worker(self):
        fixture = Fixture()
        keyboard = fixture.main.pair_ble_keyboard()
        fixture.worker.forget_peer = Mock(side_effect=RuntimeError("Pi removal failed"))
        with self.assertRaisesRegex(RuntimeError, "Pi removal failed"): keyboard.unpair()
        keyboard.close()
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
        self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_final_worker_cleanup_failure_is_not_reported_as_a_successful_close(self):
        fixture = Fixture()
        keyboard = fixture.main.pair_ble_keyboard()
        fixture.worker.close = Mock(side_effect=RuntimeError("Worker close failed"))
        with self.assertRaisesRegex(RuntimeError, "Worker close failed"): keyboard.close()
        self.assertFalse(keyboard._closed)
        self.assertIsNone(keyboard.worker)
        self.assertFalse(fixture.state["bonded"])
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)

    def test_closed_unopened_context_cannot_start_an_advertisement(self):
        fixture = Fixture()
        keyboard = PairedKeyboard(fixture.main)
        keyboard.close()
        for action in (keyboard.open, keyboard.create):
            with self.assertRaisesRegex(RuntimeError, "cannot be reused"): action()
        self.assertEqual(fixture.worker_calls, [])

    def test_unpair_waits_for_late_pi_disconnect_without_repeating_ui_operation(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            original_request, original_status = fixture.main.request, fixture.worker.status
            pending = [True, True, False]
            observed = []

            def request(path, data=None, **kwargs):
                result = original_request(path, data, **kwargs)
                if path == "/api/remotes/unpair": fixture.worker.connected = True
                return result

            def status():
                if fixture.state["remote_generation"] == "2" and pending:
                    fixture.worker.connected = pending.pop(0)
                    observed.append(fixture.worker.connected)
                return original_status()

            fixture.main.request, fixture.worker.status = request, status
            keyboard.unpair()
            self.assertEqual(observed, [True, True, False])
            self.assertIn(("forget_peer",), fixture.worker_calls)
            fixture.main.request, fixture.worker.status = original_request, original_status
            keyboard.pair().assert_paired()  # The same advertisement pairs again.
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 2)

    def test_late_pi_disconnect_wait_retains_boot_generation_and_deadline_fences(self):
        for changed in ("boot", "generation", "deadline"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                keyboard = fixture.main.pair_ble_keyboard()
                original_request, original_status = fixture.main.request, fixture.worker.status

                def request(path, data=None, **kwargs):
                    result = original_request(path, data, **kwargs)
                    if path == "/api/remotes/unpair": fixture.worker.connected = True
                    return result

                def status():
                    if fixture.state["remote_generation"] == "2":
                        if changed == "boot": fixture.system["boot_id"] = "c" * 32
                        if changed == "generation": fixture.state["remote_generation"] = "99"
                    return original_status()

                fixture.main.request, fixture.worker.status = request, status
                if changed == "deadline":
                    keyboard._pause = Mock(side_effect=TimeoutError("Pi disconnect deadline"))
                with self.assertRaises((KeyboardConflict, TimeoutError)): keyboard.unpair()
                self.assertNotIn(("forget_peer",), fixture.worker_calls)
                keyboard.close()
                self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
                self.assertEqual(fixture.worker_calls[-1], ("close",))

    def pending_unready_pair(self, fixture):
        keyboard = PairedKeyboard(fixture.main)
        original = fixture.main.request

        def request(path, data=None, **kwargs):
            result = original(path, data, **kwargs)
            if path == "/api/remotes/pair": fixture.worker.ready = False
            return result

        fixture.main.request = request
        keyboard._pause = Mock(side_effect=TimeoutError("HID readiness deadline"))
        with self.assertRaisesRegex(TimeoutError, "HID readiness deadline"): keyboard.create()
        self.assertIsNotNone(keyboard._pair_pending)
        return keyboard

    def test_failed_hid_readiness_cleans_its_proven_owned_bond_without_sending_unready_reports(self):
        fixture = Fixture()
        keyboard = self.pending_unready_pair(fixture)
        fixture.worker.report = Mock(side_effect=AssertionError("No unsubscribed HID report"))
        keyboard.close()
        fixture.worker.report.assert_not_called()
        self.assertFalse(fixture.state["bonded"] or fixture.worker.bonded)
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
        phases = [row["phase"] for row in keyboard._record["checks"]]
        self.assertIn("pair_owned_for_cleanup", phases)
        self.assertNotIn("paired", phases)
        self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_cleanup_ownership_never_uses_changed_boot_generation_or_unowned_ambiguous_pi_peer(self):
        for changed in ("boot", "generation", "unowned", "ambiguous"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                keyboard = self.pending_unready_pair(fixture)
                if changed == "boot": fixture.system["boot_id"] = "c" * 32
                if changed == "generation": fixture.state["remote_generation"] = "99"
                if changed == "unowned": fixture.worker.peer_owned = False
                if changed == "ambiguous": fixture.worker.status = Mock(side_effect=RuntimeError("Ambiguous peer"))
                with self.assertRaises((KeyboardConflict, RuntimeError)): keyboard.close()
                self.assertNotIn("/api/remotes/unpair", [path for path, _ in fixture.calls])
                self.assertNotIn("pair_owned_for_cleanup", [row["phase"] for row in keyboard._record["checks"]])
                self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_existing_bond_or_busy_remote_is_never_replaced_or_advertised_over(self):
        for key in ("bonded", "connected", "pairing", "unpairing", "connecting", "scanning"):
            with self.subTest(key=key):
                fixture = Fixture()
                fixture.state[key] = True
                keyboard = PairedKeyboard(fixture.main)
                with self.assertRaises(KeyboardConflict):
                    keyboard.create()
                keyboard.close()
                self.assertEqual(fixture.worker_calls, [])
                self.assertFalse(any(data is not None for _, data in fixture.calls))

    def test_pair_waits_for_the_untargeted_search_without_another_pair(self):
        fixture = Fixture()
        fixture.search_reads = 3
        with fixture.main.pair_ble_keyboard():
            self.assertEqual([row["pairing"] for row in fixture.pairing_statuses], [True] * 4)
            pairs = [data for path, data in fixture.calls if path == "/api/remotes/pair"]
            self.assertEqual(len(pairs), 1)
            self.assertEqual(pairs[0]["expected_remote_generation"], "0")
        self.assertEqual(fixture.state["remote_generation"], "2")

    def test_search_that_finds_no_eligible_keyboard_reports_a_distant_one_without_unpair(self):
        for too_far in (False, True):
            with self.subTest(too_far=too_far):
                fixture = Fixture()
                fixture.search_reads, fixture.too_far, fixture.terminal_pair_error = 2, too_far, 13
                keyboard = PairedKeyboard(fixture.main)
                with self.assertRaisesRegex(PairingFailed, "error 13") as raised:
                    keyboard.create()
                self.assertEqual(raised.exception.last_error, 13)
                self.assertEqual("signal strength" in str(raised.exception), too_far)
                keyboard.close()
                paths = [path for path, _ in fixture.calls]
                self.assertEqual(paths.count("/api/remotes/pair"), 1)
                self.assertNotIn("/api/remotes/unpair", paths)

    def test_bond_with_another_nearby_remote_is_reported_and_never_removed(self):
        fixture = Fixture()
        fixture.other_remote = True
        keyboard = PairedKeyboard(fixture.main)
        with self.assertRaisesRegex(KeyboardConflict, "other than the test keyboard"):
            keyboard.create()
        self.assertTrue(fixture.state["bonded"])
        with self.assertRaises((KeyboardConflict, TimeoutError)):
            keyboard.close()
        self.assertNotIn("/api/remotes/unpair", [path for path, _ in fixture.calls])
        self.assertTrue(fixture.state["bonded"])
        self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_original_main_authority_and_same_boot_are_required(self):
        for mutation in ("authority", "member", "identity", "boot"):
            with self.subTest(mutation=mutation):
                fixture = Fixture()
                keyboard = PairedKeyboard(fixture.main)
                if mutation == "authority":
                    fixture.system["membership"]["authority_ready"] = False
                elif mutation == "member":
                    fixture.system["membership"]["member"] = True
                elif mutation == "identity":
                    fixture.system["octopus_id"] = "c" * 32
                else:
                    fixture.state["boot_id"] = "c" * 32
                with self.assertRaises(KeyboardConflict):
                    keyboard.create()
                self.assertEqual(fixture.worker_calls, [])

    def test_disconnect_leaves_buttons_down_and_requires_ordinary_dut_link_cleanup(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            keyboard.report(consumer=0xe9)
            before = len(fixture.worker_calls)
            action = keyboard.disconnect()
            self.assertEqual(fixture.worker_calls[before:], [("disconnect",)])
            self.assertEqual(action.kind, "ble_disconnect")
            keyboard.assert_disconnected()
            self.assertTrue(fixture.state["bonded"])
            self.assertFalse(fixture.state["connected"])
        self.assertFalse(fixture.state["bonded"])
        self.assertEqual([row for row in fixture.worker_calls if row[0] == "report"], [("report", (), 0xe9)])

    def test_unreported_link_drops_wait_for_the_main_to_reconnect_by_itself(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            action = keyboard.drop_unreported_links(3)
            self.assertEqual(action.kind, "ble_early_link_loss")
            self.assertEqual([drop["number"] for drop in action.operation["drops"]], [1, 2, 3])
            self.assertIn(("drop_links", 3), fixture.worker_calls)
            keyboard.assert_paired()
            keyboard.report(consumer=0xe9)
        phases = [row["phase"] for row in fixture.bed.control_observations[0]["checks"]]
        self.assertEqual(phases[phases.index("early_link_loss") + 1], "reconnected")
        # Recovery is the main's own reconnect, never another pairing.
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/pair"), 1)

    def test_unreported_link_drops_fail_when_the_main_stops_reconnecting(self):
        # A main that stops reconnecting fails whatever the drops' timing.
        for stalled, drop, message in (("helper", link_drop(1), "stopped reconnecting after 1 of 3"),
                                       ("helper", link_drop(1, reaction_ns=45_000_000, encrypted=True),
                                        "stopped reconnecting after 1 of 3"),
                                       ("ui", None, "did not reconnect")):
            with self.subTest(stalled=stalled, drop=drop):
                fixture = Fixture()
                fixture.reconnect_after_drops = False
                if stalled == "helper":
                    fixture.drop_receipt = {"disconnected_monotonic_ns": 1, "completed": False, "drops": [drop]}
                with fixture.main.pair_ble_keyboard() as keyboard, self.assertRaisesRegex(AssertionError, message):
                    keyboard.drop_unreported_links(3)
                self.assertFalse(fixture.state["bonded"])

    def test_one_prompt_drop_suffices_but_late_or_unrecorded_drops_are_fixture_failures(self):
        slow = 20_000_001
        cases = (
            ({1: {"reaction_ns": 45_000_000}, 3: {"reaction_ns": slow}}, None, None),
            ({2: {"encrypted": True}}, RuntimeError, "link 2 after the main started encryption"),
            ({number: {"reaction_ns": slow} for number in (1, 2, 3)}, RuntimeError, "No link was dropped within 20 ms"),
            ({"drops": [link_drop(1), {**link_drop(2), "encrypted": 0}, link_drop(3)]}, KeyboardConflict, None),
            ({"drops": [link_drop(1), link_drop(2)]}, KeyboardConflict, None),
            ({"completed": "yes"}, KeyboardConflict, None),
            ({"drops": [{key: value for key, value in link_drop(1).items() if key != "reason"},
                        link_drop(2), link_drop(3)]}, KeyboardConflict, None))
        for changes, expected, message in cases:
            with self.subTest(changes=changes):
                fixture = Fixture()
                receipt = {"disconnected_monotonic_ns": 1, "completed": True,
                           "drops": [link_drop(number) for number in (1, 2, 3)]}
                for key, change in changes.items():
                    if isinstance(key, int):
                        receipt["drops"][key - 1].update(change)
                    else:
                        receipt[key] = change
                fixture.drop_receipt = receipt
                with fixture.main.pair_ble_keyboard() as keyboard:
                    if expected is None:
                        keyboard.drop_unreported_links(3)
                        keyboard.assert_paired()
                        continue
                    with self.assertRaises(RuntimeError) as raised:
                        keyboard.drop_unreported_links(3)
                self.assertIs(type(raised.exception), expected)
                if message is not None:
                    self.assertIn(message, str(raised.exception))
                    # Timing is judged only after the main has reconnected.
                    phases = [row["phase"] for row in fixture.bed.control_observations[0]["checks"]]
                    self.assertIn("reconnected", phases)

    def test_link_drops_need_a_connected_keyboard_and_a_bounded_count(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            for count in (0, 6, True):
                with self.subTest(count=count), self.assertRaises(ValueError):
                    keyboard.drop_unreported_links(count)
            keyboard.disconnect()
            with self.assertRaisesRegex(RuntimeError, "Connect this test keyboard"):
                keyboard.drop_unreported_links(3)
        self.assertNotIn("drop_links", [call[0] for call in fixture.worker_calls])

    def test_pair_reply_loss_is_read_only_reconciled_then_unpaired_without_duplicate_pair(self):
        fixture = Fixture()
        fixture.lose_pair_reply = True
        keyboard = PairedKeyboard(fixture.main)
        with self.assertRaises(OSError):
            keyboard.create()
        keyboard.close()
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/pair"), 1)
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
        self.assertFalse(fixture.state["bonded"])

    def test_failed_cec_session_still_cleans_only_its_owned_keyboard_bond(self):
        for lost_reply in (False, True):
            with self.subTest(lost_reply=lost_reply):
                fixture = Fixture()
                keyboard = PairedKeyboard(fixture.main)
                fixture.lose_pair_reply = lost_reply
                if lost_reply:
                    with self.assertRaises(OSError):
                        keyboard.create()
                else:
                    keyboard.create()

                def failed_cec():
                    raise ArithmeticError("Synthetic CEC worker failure")

                fixture.session.assert_healthy = failed_cec
                if not lost_reply:
                    with self.assertRaises(ArithmeticError):
                        keyboard.report(keyboard=(0x28,))
                keyboard.close()
                self.assertFalse(fixture.state["bonded"] or fixture.state["connected"])
                self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/pair"), 1)
                self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
                self.assertEqual(fixture.worker_calls[-1], ("close",))
                self.assertIsNone(keyboard.worker)

    def test_unhealthy_cec_never_bypasses_boot_authority_generation_or_bed_cleanup_fences(self):
        for changed in ("boot", "authority", "generation", "bed"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                keyboard = fixture.main.pair_ble_keyboard()
                fixture.session.assert_healthy = Mock(side_effect=ArithmeticError("CEC failure"))
                if changed == "boot":
                    fixture.system["boot_id"] = "c" * 32
                elif changed == "authority":
                    fixture.system["membership"]["authority_ready"] = False
                elif changed == "generation":
                    fixture.state["remote_generation"] = "99"
                else:
                    fixture.bed._closed = True
                with self.assertRaises(KeyboardConflict):
                    keyboard.close()
                self.assertFalse(any(path == "/api/remotes/unpair" for path, _ in fixture.calls))
                self.assertTrue(fixture.state["bonded"])
                self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_terminal_pair_failure_stops_polling_and_only_cleans_the_new_pi_bond(self):
        for error in (13, 1292):
            with self.subTest(error=error):
                fixture = Fixture()
                fixture.terminal_pair_error = error
                keyboard = PairedKeyboard(fixture.main)
                with patch.object(keyboard, "_pause", side_effect=AssertionError("A terminal failure must not poll")):
                    with self.assertRaisesRegex(PairingFailed, "error " + str(error)) as raised:
                        keyboard.create()
                self.assertEqual(raised.exception.last_error, error)
                self.assertNotIsInstance(raised.exception, KeyboardConflict)
                self.assertIsNone(keyboard._pair_pending)
                self.assertFalse(fixture.state["bonded"])
                self.assertTrue(fixture.worker.bonded)
                keyboard.close()
                keyboard.close()
                self.assertFalse(fixture.worker.bonded)
                self.assertEqual(fixture.worker_calls, [("open",), ("close",)])
                paths = [path for path, _ in fixture.calls]
                self.assertEqual(paths.count("/api/remotes/pair"), 1)
                self.assertNotIn("/api/remotes/unpair", paths)
                record = fixture.bed.control_observations[0]["checks"][-1]
                self.assertEqual(record["phase"], "pair_failed")
                self.assertEqual(record["last_error"], error)

    def test_rejected_pair_does_not_authorize_unpair_and_worker_is_closed(self):
        fixture = Fixture()
        fixture.reject_pair = True
        keyboard = PairedKeyboard(fixture.main)
        with self.assertRaises(AmityHTTPError):
            keyboard.create()
        keyboard.close()
        self.assertFalse(any(path == "/api/remotes/unpair" for path, _ in fixture.calls))
        self.assertEqual(fixture.worker_calls, [("open",), ("close",)])

    def test_uncertain_unpair_is_not_resent_and_cleanup_can_reconcile_after_worker_closes(self):
        fixture = Fixture()
        keyboard = fixture.main.pair_ble_keyboard()
        fixture.lose_unpair_reply = True
        with self.assertRaises(OSError):
            keyboard.close()
        keyboard.close()
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
        self.assertFalse(fixture.state["bonded"])

    def test_exited_worker_does_not_prevent_removing_the_owned_dut_bond(self):
        fixture = Fixture()
        keyboard = fixture.main.pair_ble_keyboard()

        def exited():
            raise EOFError("BLE keyboard worker exited")

        fixture.worker.status = exited
        fixture.worker.report = lambda **kwargs: exited()
        keyboard.close()
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
        self.assertFalse(fixture.state["bonded"])
        self.assertIsNone(keyboard.worker)

    def test_cleanup_retry_after_worker_close_still_removes_the_owned_dut_bond(self):
        fixture = Fixture()
        keyboard = fixture.main.pair_ble_keyboard()
        original = fixture.request
        failures = [OSError("Remote status read failed")]

        def request(path, data=None, **kwargs):
            if path == "/api/remotes" and data is None and failures:
                raise failures.pop()
            return original(path, data, **kwargs)

        fixture.main.request = request
        with self.assertRaisesRegex(OSError, "status read failed"):
            keyboard.close()
        self.assertIsNone(keyboard.worker)
        self.assertTrue(fixture.state["bonded"])
        keyboard.close()
        self.assertEqual([path for path, _ in fixture.calls].count("/api/remotes/unpair"), 1)
        self.assertFalse(fixture.state["bonded"])

    def test_changed_remote_generation_prevents_cleanup_overwriting_external_work(self):
        fixture = Fixture()
        keyboard = fixture.main.pair_ble_keyboard()
        fixture.state["remote_generation"] = "99"
        with self.assertRaises(KeyboardConflict):
            keyboard.close()
        self.assertTrue(fixture.state["bonded"])
        self.assertFalse(any(path == "/api/remotes/unpair" for path, _ in fixture.calls))

    def test_invalid_report_cannot_cross_the_worker_boundary(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            for keys, consumer in (((1, 1), 0), (tuple(range(1, 8)), 0), ((True,), 0), ((), 65536)):
                with self.subTest(keys=keys, consumer=consumer), self.assertRaises(ValueError):
                    keyboard.report(keyboard=keys, consumer=consumer)
            self.assertFalse(any(row[0] == "report" for row in fixture.worker_calls))

    def test_six_ordinary_keys_and_all_modifiers_can_overlap_a_consumer_key(self):
        fixture = Fixture()
        usages = (*range(4, 10), *range(0xe0, 0xe8))
        with fixture.main.pair_ble_keyboard() as keyboard:
            keyboard.report(keyboard=usages, consumer=0xe9)
            self.assertEqual(fixture.worker_calls[-1], ("report", usages, 0xe9))
            for usage in (0, 1, 2, 3, 0xe8, 255):
                with self.subTest(usage=usage), self.assertRaises(ValueError):
                    keyboard.report(keyboard=(usage,))

    def test_report_context_sends_fresh_hid_states_without_repeating_http_preflight(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            fixture.main.get_system = Mock(wraps=fixture.main.get_system)
            fixture.main.get_state = Mock(wraps=fixture.main.get_state)
            fixture.main.last_scan.validate_owners = Mock()
            before = len(fixture.calls)
            with keyboard.reports() as reports:
                counts = (fixture.main.get_system.call_count, fixture.main.get_state.call_count,
                          fixture.main.last_scan.validate_owners.call_count, len(fixture.calls))
                self.assertEqual(counts[:3], (1, 1, 1))
                self.assertEqual(fixture.calls[before:], [("/api/remotes", None)])
                snapshot = fixture.main.last_scan.validate_owners.call_args.kwargs["_system"]
                self.assertEqual(snapshot, fixture.system)
                first = reports.report(keyboard=(0x28,))
                fixture.activity.update(current_activity=1, activity_settling=True)
                second = reports.report(consumer=0xe9)
                released = reports.all_up()
                self.assertEqual((fixture.main.get_system.call_count, fixture.main.get_state.call_count,
                                  fixture.main.last_scan.validate_owners.call_count, len(fixture.calls)), counts)
                self.assertLess(first.started_ns, second.started_ns)
                self.assertLess(second.started_ns, released.started_ns)
                self.assertEqual(len({first.request_id, second.request_id, released.request_id}), 3)
                self.assertEqual(first.state["current_activity"], second.state["current_activity"])
                self.assertEqual(second.state["current_activity"], 0)
                self.assertFalse(second.state["activity_settling"])
                self.assertIs(second.scan, fixture.main.last_scan)
                self.assertEqual(second.owner_boots, fixture.main.last_scan.owner_boots)
                first.state["expected_config_revision"] = "caller mutation"
                self.assertEqual(second.state["expected_config_revision"], "4")
            self.assertEqual((fixture.main.get_system.call_count, fixture.main.get_state.call_count,
                              fixture.main.last_scan.validate_owners.call_count), (2, 2, 2))
            self.assertEqual(fixture.calls[before:], [("/api/remotes", None), ("/api/remotes", None)])
            self.assertIsNot(fixture.main.last_scan.validate_owners.call_args.kwargs["_system"], snapshot)
            self.assertEqual([call for call in fixture.worker_calls if call[0] == "report"],
                             [("report", (0x28,), 0), ("report", (), 0xe9),
                              ("report", (), 0), ("report", (), 0)])

    def test_report_context_releases_held_input_and_preserves_original_body_failure(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            with self.assertRaisesRegex(ValueError, "original failure"):
                with keyboard.reports() as reports:
                    reports.report(consumer=0xe9)
                    raise ValueError("original failure")
            self.assertEqual(fixture.worker_calls[-1], ("report", (), 0))
            self.assertTrue(fixture.state["bonded"])
            with self.assertRaises(KeyboardConflict):
                reports.report(keyboard=(0x28,))

    def test_fast_report_does_not_wait_for_the_cec_responder_lock(self):
        fixture = Fixture()
        session = CecSession(())
        fixture.session = fixture.bed.session = fixture.main.last_scan._session = session
        session.assert_healthy = Mock(wraps=session.assert_healthy)
        locked, release, reported = threading.Event(), threading.Event(), threading.Event()
        actions, failures = [], []

        def responder():
            with session._lock:
                locked.set()
                release.wait(2)

        with fixture.main.pair_ble_keyboard() as keyboard:
            with keyboard.reports() as reports:
                checked = session.assert_healthy.call_count

                def notify():
                    try:
                        actions.append(reports.report(keyboard=(0x28,)))
                    except BaseException as error:
                        failures.append(error)
                    finally:
                        reported.set()

                holder = threading.Thread(target=responder)
                sender = threading.Thread(target=notify)
                holder.start()
                try:
                    self.assertTrue(locked.wait(1))
                    sender.start()
                    self.assertTrue(reported.wait(1), "BLE notification waited for the CEC responder")
                    self.assertEqual(session.assert_healthy.call_count, checked)
                    self.assertEqual(failures, [])
                    self.assertEqual(len(actions), 1)
                    self.assertEqual(fixture.worker_calls[-1], ("report", (0x28,), 0))
                finally:
                    release.set()
                    holder.join(timeout=1)
                    if sender.ident is not None:
                        sender.join(timeout=1)
            self.assertGreater(session.assert_healthy.call_count, checked)

    def test_stopped_fast_reports_reject_notification_but_still_release_owned_hid_state(self):
        for failed in (False, True):
            with self.subTest(failed=failed):
                fixture = Fixture()
                session = CecSession(())
                fixture.session = fixture.bed.session = fixture.main.last_scan._session = session
                with fixture.main.pair_ble_keyboard() as keyboard:
                    expected_error = CecError if failed else KeyboardConflict
                    with self.assertRaises(expected_error):
                        with keyboard.reports() as reports:
                            reports.report(consumer=0xe9)
                            before = len(fixture.worker_calls)
                            with session._lock:
                                if failed:
                                    session._error = ArithmeticError("CEC responder failure")
                                session._stop.set()
                            with self.assertRaises(KeyboardConflict):
                                reports.report(keyboard=(0x28,))
                            self.assertEqual(len(fixture.worker_calls), before)
                    self.assertEqual(fixture.worker_calls[before:], [("report", (), 0)])
                self.assertFalse(fixture.state["bonded"])
                self.assertEqual(fixture.worker_calls[-1], ("close",))

    def test_report_context_fences_configuration_boot_and_owner_changes_on_exit(self):
        for changed in ("expected_config_revision", "expected_presentation_revision", "main_boot", "owner_boot"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                with fixture.main.pair_ble_keyboard() as keyboard:
                    original_system = deepcopy(fixture.system)
                    original_owners = dict(fixture.main.last_scan.owner_boots)
                    try:
                        with self.assertRaises(KeyboardConflict):
                            with keyboard.reports() as reports:
                                reports.report(consumer=0xe9)
                                if changed == "main_boot":
                                    fixture.system["boot_id"] = "c" * 32
                                elif changed == "owner_boot":
                                    fixture.main.last_scan.owner_boots[fixture.main.identity.device_id] = "c" * 32
                                else:
                                    fixture.activity[changed] = "99"
                        self.assertEqual(fixture.worker_calls[-1], ("report", (), 0))
                    finally:
                        fixture.system = original_system
                        fixture.main.last_scan.owner_boots = original_owners

    def test_each_fast_report_requires_the_same_live_scan_bed_worker_and_connection(self):
        for changed in ("scan", "bed", "worker", "disconnected"):
            with self.subTest(changed=changed):
                fixture = Fixture()
                with fixture.main.pair_ble_keyboard() as keyboard:
                    with keyboard.reports() as reports:
                        original_scan = fixture.main.last_scan
                        original_worker = keyboard.worker
                        before = len(fixture.worker_calls)
                        try:
                            if changed == "scan":
                                fixture.main.last_scan = object()
                            elif changed == "bed":
                                fixture.bed._closed = True
                            elif changed == "worker":
                                keyboard.worker = object()
                            else:
                                keyboard._disconnected = True
                            with self.assertRaises(KeyboardConflict):
                                reports.report(keyboard=(0x28,))
                            self.assertEqual(len(fixture.worker_calls), before)
                        finally:
                            fixture.main.last_scan, keyboard.worker = original_scan, original_worker
                            fixture.bed._closed = keyboard._disconnected = False

    def test_report_context_uses_physical_worker_receipt_validation(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            with keyboard.reports() as reports:
                with patch.object(fixture.worker, "report", return_value={
                        "timestamp_monotonic_ns": True, "keyboard_report": "0000000000000000",
                        "consumer_report": "0000"}):
                    with self.assertRaisesRegex(KeyboardConflict, "physical notification receipt"):
                        reports.report(keyboard=(0x28,))
            self.assertEqual(fixture.worker_calls[-1], ("report", (), 0))

    def test_report_context_rejects_missing_revision_and_cannot_be_reentered(self):
        fixture = Fixture()
        with fixture.main.pair_ble_keyboard() as keyboard:
            fixture.activity.pop("expected_config_revision")
            with self.assertRaisesRegex(KeyboardConflict, "configuration revisions"):
                with keyboard.reports():
                    self.fail("An unfenced report context must not open")
            fixture.activity["expected_config_revision"] = "4"
            reports = keyboard.reports()
            with reports:
                with self.assertRaises(RuntimeError):
                    reports.__enter__()
            with self.assertRaises(RuntimeError):
                reports.__enter__()


class KeyboardTransportTests(unittest.TestCase):
    def test_cli_uses_local_keyboard_in_the_runner_environment(self):
        captured = []

        def run(test, config):
            captured.append(config)
            return {"test": "keyboard", "passed": True, "testbeds": []}

        with patch("amity_harness.runner.run_test", side_effect=run), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            result = run_main(lambda: None, ["--credentials", "/unused/private.json", "--no-status-led"])
        self.assertEqual(result, 0)
        worker = captured[0].keyboard_factory()
        self.assertIsInstance(worker, LocalKeyboard)
        self.assertEqual(worker.python, sys.executable)
        self.assertEqual(captured[0].session_factory, CecSession)
        with self.assertRaises(ValueError):
            RunConfig(keyboard_factory=42)

    def test_link_drop_reply_deadline_covers_every_requested_link(self):
        worker = LocalKeyboard(timeout=20)
        with patch.object(worker, "_rpc", return_value={}) as rpc:
            worker.drop_links(3)
        rpc.assert_called_once_with("drop_links", {"count": 3}, timeout=20 + 3 * (LINK_DROP_TIMEOUT + 5))

    def test_correlated_worker_protocol_rejects_wrong_id_and_is_always_reaped(self):
        for wrong in (False, True):
            with self.subTest(wrong=wrong), tempfile.TemporaryDirectory() as directory:
                script = Path(directory) / "worker.py"
                script.write_text("""import json,sys
for line in sys.stdin:
 r=json.loads(line)
 result={'protocol_version':1,'address':'12:34:56:78:9a:bc','type':0,'name':'Unit Keyboard'} if r['operation']=='open' else {}
 print(json.dumps({'id':r['id']+WRONG,'ok':True,'result':result}),flush=True)
 if r['operation']=='close':break
""".replace("WRONG", "1" if wrong else "0"))
                worker = LocalKeyboard(timeout=.5)
                worker._command = lambda: [sys.executable, "-u", str(script)]
                if wrong:
                    with self.assertRaisesRegex(RuntimeError, "reply identity"):
                        worker.open()
                else:
                    self.assertEqual(worker.open()["name"], "Unit Keyboard")
                    worker.close()
                self.assertIsNone(worker._process)

    def test_worker_rejection_names_only_its_exception_type(self):
        for error, detail in (("TimeoutError: not subscribed to private/dbus/path", " (TimeoutError)"),
                              ("dbus_fast.errors.DBusError: private detail", " (dbus_fast.errors.DBusError)"),
                              ("KeyError", " (KeyError)"), ("not a type: private", ""), (None, "")):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                script = Path(directory) / "worker.py"
                script.write_text("""import json,sys
for line in sys.stdin:
 r=json.loads(line)
 if r['operation']=='open':
  print(json.dumps({'id':r['id'],'ok':True,'result':{'protocol_version':1,'address':'12:34:56:78:9a:bc','type':0,'name':'Unit Keyboard'}}),flush=True)
 else:
  print(json.dumps({'id':r['id'],'ok':False,'error':ERROR}),flush=True)
  break
""".replace("ERROR", repr(error)))
                worker = LocalKeyboard(timeout=.5)
                worker._command = lambda: [sys.executable, "-u", str(script)]
                worker.open()
                try:
                    with self.assertRaises(RuntimeError) as raised:
                        worker.status()
                finally:
                    worker._shutdown()
                self.assertEqual(str(raised.exception), "BLE keyboard worker rejected status" + detail)
                self.assertNotIn("private", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
