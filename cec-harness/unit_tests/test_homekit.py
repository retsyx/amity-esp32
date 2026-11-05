"""HomeKit pairing ownership, dynamic characteristic lookup and raw HAP writes."""

import asyncio
from copy import deepcopy
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.homekit import HomeKitAccessory, HomeKitError, _Layout, _Session, _Worker, _uuid


def accessory_graph(serial="68EE8F502B44"):
    iid = 100

    def service(kind, pairs):
        nonlocal iid
        chars = []
        for kind_char, value in pairs:
            iid += 3
            chars.append({"type": kind_char, "iid": iid, "value": value})
        return {"type": kind, "characteristics": chars}

    return [{"aid": 41, "services": [
        service("3E", [("30", serial)]),
        service("D8", [("B0", 0), ("E7", 0), ("E1", 0)]),
        service("113", [("B0", 0), ("11A", False), ("EA", 0)]),
        service("D9", [("E6", 8), ("E3", "Watch"), ("D6", 1), ("135", 0)]),
        service("D9", [("E6", 9), ("E3", "Unused"), ("D6", 0), ("135", 1)]),
        service("D9", [("E6", 10), ("E3", "Hidden"), ("D6", 1), ("135", 1)]),
    ]}]


class FakeDUT:
    name = "amity-dut"
    url = "https://amity.local"
    identity = SimpleNamespace(device_id="a" * 32, mac="68:EE:8F:50:2B:44")
    _runtime = SimpleNamespace(main_dut="amity-dut", timeout=1)

    def __init__(self):
        self.state = {"available": True, "started": False, "pairing": False, "busy": False,
                      "paired": False, "controllers": 0}
        self.requests = []

    def request(self, path, body=None):
        self.requests.append((path, body))
        if path == "/api/homekit":
            return deepcopy(self.state)
        if path == "/api/homekit/open":
            self.state.update(pairing=True, started=True, manual_code="123-45-679")
        elif path == "/api/homekit/close":
            self.state.update(pairing=False)
        elif path == "/api/homekit/remove":
            self.state.update(paired=False, controllers=0)
        else:
            raise AssertionError("No alternate DUT control route is permitted")
        return {"ok": True}


class FakeWorker:
    def __init__(self, dut):
        self.dut = dut
        self.process = self.directory = None
        self.calls = []
        self.failed_close = False

    def start(self, identity):
        self.calls.append(("start", identity))
        self.process = object()

    def rpc(self, operation, arguments=None):
        self.calls.append((operation, deepcopy(arguments)))
        if operation == "open":
            self.dut.state.update(paired=True, pairing=False, controllers=1)
            return {"verified": True}
        if operation == "close":
            if self.failed_close:
                raise HomeKitError("Private native failure")
            self.dut.state.update(paired=False, controllers=0)
            return {"closed": True}
        if operation == "read_inputs":
            return {"inputs": [{"index": 8, "name": "Watch"}], "current_input": 8}
        if operation == "read_state":
            return {"active": True, "current_input": 8, "speaker_active": True}
        return -70410 if arguments["value"] == 255 else 0

    def stop(self):
        self.process = None
        self.calls.append(("stop", None))


class HomeKitAccessoryTests(unittest.TestCase):
    def fixture(self):
        dut = FakeDUT()
        worker = FakeWorker(dut)
        factory = Mock(return_value=worker)
        accessory = HomeKitAccessory(dut, worker_factory=factory)
        return dut, worker, accessory

    def test_pairing_uses_shipping_ui_and_correct_identity_without_logging_code(self):
        dut, worker, accessory = self.fixture()
        with patch("amity_harness.homekit.event") as trace:
            with accessory:
                self.assertTrue(dut.state["paired"])
                self.assertEqual(accessory.read_inputs(), {"inputs": [{"index": 8, "name": "Watch"}], "current_input": 8})
                self.assertEqual(accessory.read_state(), {"active": True, "current_input": 8, "speaker_active": True})
            self.assertNotIn("123-45-679", str(trace.call_args_list))
        self.assertIn(("open", {"host": "amity.local", "serial": "68EE8F502B44", "pin": "123-45-679"}), worker.calls)
        self.assertEqual([path for path, body in dut.requests if body is not None],
                         ["/api/homekit/open", "/api/homekit/close"])
        self.assertFalse(dut.state["paired"])
        self.assertFalse(dut.state["pairing"])
        self.assertIsNone(worker.process)

    def test_existing_pairing_or_setup_is_never_changed(self):
        for change in ({"paired": True, "controllers": 1}, {"controllers": 2},
                       {"pairing": True}, {"busy": True}):
            with self.subTest(change=change):
                dut, worker, accessory = self.fixture()
                dut.state.update(change)
                with self.assertRaisesRegex(HomeKitError, "preserved"):
                    accessory.__enter__()
                self.assertEqual(worker.calls, [])
                self.assertEqual(dut.requests, [("/api/homekit", None)])

    def test_raw_unsupported_keys_reach_the_worker_without_conversion(self):
        _, worker, accessory = self.fixture()
        with accessory:
            self.assertEqual(accessory.send_key(255), -70410)
            self.assertEqual(accessory.select_input(17), 0)
            self.assertEqual(accessory.set_power(False), 0)
            self.assertEqual(accessory.set_mute(True), 0)
            self.assertEqual(accessory.step_volume(1), 0)
            self.assertEqual(accessory.step_volume(-1), 0)
        self.assertIn(("send_key", {"value": 255}), worker.calls)
        self.assertIn(("select_input", {"value": 17}), worker.calls)
        self.assertIn(("power", {"value": 0}), worker.calls)
        self.assertIn(("mute", {"value": True}), worker.calls)
        self.assertIn(("volume", {"value": 0}), worker.calls)
        self.assertIn(("volume", {"value": 1}), worker.calls)

    def test_invalid_public_values_never_reach_the_worker(self):
        _, worker, accessory = self.fixture()
        with accessory:
            before = list(worker.calls)
            for function, value in ((accessory.send_key, True), (accessory.send_key, 256),
                                    (accessory.select_input, -1), (accessory.set_power, 1),
                                    (accessory.set_mute, 0), (accessory.step_volume, 0)):
                with self.assertRaises(ValueError):
                    function(value)
            self.assertEqual(worker.calls, before)

    def test_malformed_read_state_is_not_reported_as_observed(self):
        _, worker, accessory = self.fixture()
        with accessory:
            with patch.object(worker, "rpc", return_value={"active": 1, "current_input": 8, "speaker_active": True}):
                with self.assertRaises(HomeKitError):
                    accessory.read_state()

    def test_cleanup_preserves_original_scenario_failure(self):
        dut, worker, accessory = self.fixture()
        with self.assertRaisesRegex(ValueError, "original scenario"):
            with accessory:
                worker.failed_close = True
                raise ValueError("original scenario")
        self.assertNotIn(("/api/homekit/remove", {"confirm": True}), dut.requests)
        self.assertTrue(dut.state["paired"])
        self.assertFalse(dut.state["pairing"])

    def test_failed_unpair_never_uses_controller_count_as_ownership_proof(self):
        dut, worker, accessory = self.fixture()
        accessory.__enter__()
        worker.failed_close = True
        # Even a single controller after a lost reply could be a newly added
        # external pairing; removing every controller cannot prove ownership.
        self.assertEqual(dut.state["controllers"], 1)
        with self.assertRaises(HomeKitError):
            accessory.close()
        self.assertNotIn(("/api/homekit/remove", {"confirm": True}), dut.requests)
        self.assertIn(("/api/homekit/close", {}), dut.requests)
        self.assertTrue(dut.state["paired"])

    def test_cleanup_never_clears_an_additional_controller(self):
        dut, worker, accessory = self.fixture()
        accessory.__enter__()
        worker.failed_close = True
        dut.state.update(controllers=2)
        with patch.object(accessory, "_wait", side_effect=TimeoutError):
            with self.assertRaises(HomeKitError):
                accessory.close()
        self.assertNotIn(("/api/homekit/remove", {"confirm": True}), dut.requests)


class HomeKitGraphTests(unittest.TestCase):
    def test_dynamic_graph_identity_and_full_standard_uuid(self):
        graph = accessory_graph()
        graph[0]["services"][1]["type"] = "000000D8-0000-1000-8000-0026BB765291"
        layout = _Layout(graph, "68EE8F502B44")
        self.assertEqual(layout.chars["active"], (41, 106))
        self.assertEqual(layout.chars["volume"], (41, 121))
        self.assertEqual(_uuid("0000011A-0000-1000-8000-0026BB765291"), "11A")

    def test_wrong_serial_missing_characteristic_and_ambiguous_tv_are_rejected(self):
        for mutate in (lambda graph: graph[0]["services"][0]["characteristics"][0].update(value="other"),
                       lambda graph: graph[0]["services"][1]["characteristics"].pop(),
                       lambda graph: graph.append(deepcopy(graph[0]))):
            graph = accessory_graph()
            mutate(graph)
            with self.assertRaises(HomeKitError):
                _Layout(graph, "68EE8F502B44")


class HomeKitWorkerTests(unittest.TestCase):
    def worker(self):
        worker = _Worker(timeout=1)
        worker.process = Mock()
        worker.process.stdin.fileno.return_value = 101
        worker.process.stdout.fileno.return_value = 102
        return worker

    def test_ipc_preserves_raw_status_and_rejects_stale_or_failed_replies(self):
        for reply, failed in (({"id": 1, "ok": True, "result": -70410}, False),
                              ({"id": 2, "ok": True, "result": 0}, True),
                              ({"id": 1, "ok": False, "error_type": "private message"}, True)):
            worker = self.worker()
            with self.subTest(reply=reply), \
                    patch("amity_harness.homekit.select.select", side_effect=lambda reads, writes, errors, timeout: (reads, writes, [])), \
                    patch("amity_harness.homekit.os.write", side_effect=lambda fd, data: len(data)) as write, \
                    patch("amity_harness.homekit.os.read", return_value=(json.dumps(reply) + "\n").encode()):
                if failed:
                    with self.assertRaises(HomeKitError) as caught:
                        worker.rpc("send_key", {"value": 255})
                    self.assertNotIn("private message", str(caught.exception))
                else:
                    self.assertEqual(worker.rpc("send_key", {"value": 255}), -70410)
                    self.assertEqual(json.loads(bytes(write.call_args.args[1])),
                                     {"id": 1, "operation": "send_key", "arguments": {"value": 255}})

    def test_hung_worker_has_bounded_ipc_and_is_not_sent_another_key(self):
        worker = self.worker()
        with patch("amity_harness.homekit.time.monotonic", side_effect=[0, 0, 2]), \
                patch("amity_harness.homekit.os.write") as write:
            with self.assertRaises(HomeKitError):
                worker.rpc("send_key", {"value": 4})
        write.assert_not_called()
        self.assertEqual(worker.sequence, 1)
        self.assertFalse(worker.lock.locked())

    def test_stop_closes_pipes_and_reaps_the_process_group_after_deadline(self):
        worker = self.worker()
        process = worker.process
        process.wait.side_effect = subprocess.TimeoutExpired("homekit", 3)
        worker.log = Mock()
        log = worker.log
        with patch("amity_harness.homekit._stop_process_group") as stop:
            worker.stop()
        process.stdin.close.assert_called_once()
        process.stdout.close.assert_called_once()
        process.wait.assert_called_once_with(timeout=3)
        stop.assert_called_once_with(process)
        log.close.assert_called_once()
        self.assertIsNone(worker.process)


class HomeKitOperationTests(unittest.IsolatedAsyncioTestCase):
    def fixture(self, directory):
        graph = accessory_graph()
        session = _Session(directory)
        session.layout = _Layout(graph, "68EE8F502B44")
        values = {(accessory["aid"], char["iid"]): {"value": char["value"]}
                  for accessory in graph for service in accessory["services"] for char in service["characteristics"]}
        session.pairing = SimpleNamespace(
            get_characteristics=AsyncMock(side_effect=lambda locations: {key: values[key] for key in locations}),
            put_characteristics=AsyncMock(return_value={}), remove_pairing=AsyncMock(return_value=True),
            shutdown=AsyncMock(), pairing_data={"iOSPairingId": "owned-controller"})
        return session, values

    async def test_only_configured_visible_inputs_are_returned_with_live_values(self):
        with tempfile.TemporaryDirectory() as directory:
            session, values = self.fixture(directory)
            values[session.layout.chars["current_input"]] = {"value": 8}
            self.assertEqual(await session.operation("read_inputs", {}),
                             {"inputs": [{"index": 8, "name": "Watch"}], "current_input": 8})
            values[session.layout.inputs[0]["name"]] = {"value": "Renamed"}
            self.assertEqual((await session.operation("read_inputs", {}))["inputs"][0]["name"], "Renamed")

    async def test_raw_write_status_and_unknown_values_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            session, _ = self.fixture(directory)
            key = session.layout.chars["key"]
            session.pairing.put_characteristics.return_value = {key: {"status": -70410}}
            self.assertEqual(await session.operation("send_key", {"value": 255}), -70410)
            session.pairing.put_characteristics.assert_awaited_once_with([(*key, 255)])
            for result in ({key: {}}, {(99, 99): {"status": 0}}, {key: {"status": "0"}}):
                session.pairing.put_characteristics.return_value = result
                with self.assertRaises(HomeKitError):
                    await session.operation("send_key", {"value": 0})

    async def test_failed_characteristic_read_cannot_be_used_as_observed_state(self):
        with tempfile.TemporaryDirectory() as directory:
            session, values = self.fixture(directory)
            values[session.layout.chars["active"]] = {"status": -70409}
            with self.assertRaises(HomeKitError):
                await session.operation("read_state", {})

    async def test_large_input_graph_is_partitioned_before_gets_without_losing_values(self):
        with tempfile.TemporaryDirectory() as directory:
            session = _Session(directory)
            locations = [(41, 1000000000 + index) for index in range(65)]

            def read(batch):
                uri = "/characteristics?id=" + ",".join(f"{aid}.{iid}" for aid, iid in batch)
                self.assertLessEqual(len(f"GET {uri} HTTP/1.1\r\nHost: "), 256)
                return {location: {"value": index} for index, location in enumerate(batch)}

            session.pairing = SimpleNamespace(get_characteristics=AsyncMock(side_effect=read))
            values = await session._read([*locations, locations[0]])
            requested = [location for call in session.pairing.get_characteristics.call_args_list
                         for location in call.args[0]]
            self.assertEqual(requested, locations)
            self.assertEqual(set(values), set(locations))
            self.assertGreater(session.pairing.get_characteristics.await_count, 1)

    async def test_failed_batch_is_not_retried_or_used_as_a_partial_read(self):
        with tempfile.TemporaryDirectory() as directory:
            session = _Session(directory)
            locations = [(41, 1000000000 + index) for index in range(65)]
            session.pairing = SimpleNamespace(get_characteristics=AsyncMock(side_effect=TimeoutError))
            with self.assertRaises(TimeoutError):
                await session._read(locations)
            self.assertEqual(session.pairing.get_characteristics.await_count, 1)

    async def test_close_removes_only_its_exact_controller_and_deletes_saved_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            session, _ = self.fixture(directory)
            pairing = session.pairing
            path = Path(directory) / "pairing.json"
            path.write_text("private keys")
            await session.close()
            pairing.remove_pairing.assert_awaited_once_with("owned-controller")
            pairing.shutdown.assert_awaited_once()
            self.assertFalse(path.exists())

    async def test_failed_pairing_removal_keeps_private_keys_and_still_closes_session(self):
        with tempfile.TemporaryDirectory() as directory:
            session, _ = self.fixture(directory)
            pairing = session.pairing
            pairing.remove_pairing.return_value = False
            path = Path(directory) / "pairing.json"
            path.write_text("private keys")
            with self.assertRaises(HomeKitError):
                await session.close()
            pairing.remove_pairing.assert_awaited_once_with("owned-controller")
            pairing.shutdown.assert_awaited_once()
            self.assertTrue(path.exists())


if __name__ == "__main__":
    unittest.main()
