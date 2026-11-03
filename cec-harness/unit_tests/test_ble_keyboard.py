"""Real-keyboard fixture ownership and shipping UI pairing fences."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import json
import io
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout, redirect_stderr

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT
from amity_harness.ble_keyboard import KeyboardConflict, LocalKeyboard, PairedKeyboard, SSHKeyboard
from amity_harness.config import RunConfig
from amity_harness.dut import AmityHTTPError
from amity_harness.runner import main as run_main


class Fixture:
    def __init__(self):
        self.main = DUT("amity-dut")
        self.boot, self.group = "a" * 32, "b" * 32
        self.calls, self.worker_calls, self.cleanups = [], [], []
        self.system = {"octopus_id": self.main.identity.device_id, "boot_id": self.boot,
                       "consortium_id": self.group, "mode": "coordinator",
                       "membership": {"coordinator": True, "member": False, "authority_ready": True}}
        self.state = {"ready": True, "boot_id": self.boot, "remote_generation": "0", "devices": [],
                      "scanning": False, "bonded": False, "connected": False, "pairing": False,
                      "connecting": False, "unpairing": False}
        self.activity = {"expected_boot_id": self.boot, "current_activity": 0, "activity_settling": False,
                         "expected_config_revision": "4", "expected_presentation_revision": "4"}
        self.session = SimpleNamespace(assert_healthy=lambda: None)
        self.bed = SimpleNamespace(_entered=True, _closed=False, duts=(self.main,), members=(),
                                   control_only=(), session=self.session, add_cleanup=self.cleanups.append)
        self.main._testbed = self.bed
        self.worker = Worker(self)
        self.main._runtime = RunConfig(timeout=.1, keyboard_factory=lambda: self.worker)
        self.main.get_system = lambda: deepcopy(self.system)
        self.main.get_state = lambda: deepcopy(self.activity)
        self.main.request = self.request
        self.main.last_scan = SimpleNamespace(dut=self.main, _session=self.session, initiators={23: 4},
                                             owner_boots={self.main.identity.device_id: self.boot},
                                             owner_duts={self.main.identity.device_id: self.main},
                                             expected=(), validate_owners=lambda expected: None)
        self.lose_pair_reply = self.lose_unpair_reply = self.reject_pair = False

    def request(self, path, data=None, **kwargs):
        self.calls.append((path, deepcopy(data)))
        if path == "/api/remotes" and data is None:
            return deepcopy(self.state)
        if path == "/api/remotes/scan":
            self.state["devices"] = [{"address": "12:34:56:78:9a:bc", "name": "Amity Test Keyboard", "type": 0}]
            self.state["scanning"] = True
            return {"saved": True}
        if path not in ("/api/remotes/pair", "/api/remotes/unpair"):
            raise AssertionError("Unexpected DUT route: " + path)
        self.assert_payload(path, data)
        if self.reject_pair and path.endswith("/pair"):
            raise AmityHTTPError(409, "Changed pairing review")
        self.state["remote_generation"] = str(int(self.state["remote_generation"]) + 1)
        paired = path.endswith("/pair")
        self.state.update(bonded=paired, connected=paired, scanning=False)
        self.worker.connected = self.worker.bonded = self.worker.ready = paired
        if paired and self.lose_pair_reply:
            self.lose_pair_reply = False
            raise OSError("Pair reply lost")
        if not paired and self.lose_unpair_reply:
            self.lose_unpair_reply = False
            raise OSError("Unpair reply lost")
        return {"saved": True, "admitted": True, "duplicate": False,
                "remote_generation": self.state["remote_generation"]}

    def assert_payload(self, path, data):
        assert data["expected_boot_id"] == self.boot
        assert data["expected_remote_generation"] == self.state["remote_generation"]
        assert len(data["request_id"]) == 32
        assert set(data) == {"request_id", "expected_boot_id", "expected_remote_generation"} | (
            {"address", "type"} if path.endswith("/pair") else set())
        if path.endswith("/pair"):
            assert data["address"] == "12:34:56:78:9a:bc" and data["type"] == 0


class Worker:
    def __init__(self, fixture):
        self.fixture = fixture
        self.connected = self.bonded = self.ready = False

    def open(self):
        self.fixture.worker_calls.append(("open",))
        return {"address": "12:34:56:78:9a:bc", "type": 0, "name": "Amity Test Keyboard", "protocol_version": 1}

    def status(self):
        return {"ready": self.ready, "connected": self.connected, "bonded": self.bonded}

    def report(self, *, keyboard=(), consumer=0):
        self.fixture.worker_calls.append(("report", tuple(keyboard), consumer))
        assert self.connected and self.ready
        return {"timestamp_monotonic_ns": time.monotonic_ns(), "keyboard_report": "0000000000000000",
                "consumer_report": "0000", "changed": [1, 2]}

    def disconnect(self):
        self.fixture.worker_calls.append(("disconnect",))
        self.connected = self.ready = False
        self.fixture.state["connected"] = False
        return {"disconnected": True}

    def close(self):
        self.fixture.worker_calls.append(("close",))
        self.connected = self.ready = self.bonded = False


class PairedKeyboardTests(unittest.TestCase):
    def setUp(self):
        sleeper = patch("amity_harness.ble_keyboard.time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def test_exact_target_pair_and_owned_cleanup_use_fresh_shipping_ui_reviews(self):
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
        self.assertNotIn("/api/key", paths)
        self.assertFalse(fixture.state["bonded"])
        self.assertEqual(fixture.state["remote_generation"], "2")
        phases = [row["phase"] for row in fixture.bed.control_observations[0]["checks"]]
        self.assertEqual(phases, ["pair_submit", "paired", "report", "unpair_submit", "unpaired"])

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


class KeyboardTransportTests(unittest.TestCase):
    def test_cli_keyboard_python_selects_only_keyboard_worker(self):
        captured = []

        def run(test, config):
            captured.append(config)
            return {"test": "keyboard", "passed": True, "testbeds": []}

        with patch("amity_harness.runner.run_test", side_effect=run), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            result = run_main(lambda: None, ["--credentials", "/unused/private.json", "--harness-host", "pi@bench",
                                            "--harness-root", "/home/pi/harness", "--ble-python", "/home/pi/ble/bin/python",
                                            "--ssh-port", "2222", "--no-status-led"])
        self.assertEqual(result, 0)
        worker = captured[0].keyboard_factory()
        self.assertIsInstance(worker, SSHKeyboard)
        self.assertEqual(worker.python, "/home/pi/ble/bin/python")
        self.assertEqual(worker.port, 2222)
        cec_factory = captured[0].session_factory.keywords["backend_factory"]
        self.assertNotIn("python", cec_factory.keywords)

    def test_ssh_command_quotes_remote_paths_and_never_contains_dut_authentication(self):
        keyboard = SSHKeyboard(host="pi@test2.local", root="/home/pi/my harness", port=2222,
                               python="/home/pi/keyboard env/bin/python")
        command = keyboard._command()
        self.assertEqual(command[:2], ["ssh", "-T"])
        self.assertIn("2222", command)
        self.assertIn("'PYTHONPATH=/home/pi/my harness'", command[-1])
        self.assertIn("'/home/pi/keyboard env/bin/python'", command[-1])
        self.assertNotIn("password", " ".join(command).lower())
        with self.assertRaises(ValueError):
            SSHKeyboard(host="-ProxyCommand=bad", root="/tmp")
        with self.assertRaises(ValueError):
            RunConfig(keyboard_factory=42)

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


if __name__ == "__main__":
    unittest.main()
