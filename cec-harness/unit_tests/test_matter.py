"""Saved-fabric isolation and real accessory command translation."""

import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.keys import Key
from amity_harness.config import RunConfig
from amity_harness.matter import MatterAccessory, MatterError, _operate, _validate_request, _worker
from amity_harness.matter import _Worker, _check_reply, _native_session, _reply, _store_reply


DEVICE = "0123456789abcdef0123456789abcdef"


class MatterAccessoryTests(unittest.TestCase):
    def accessory(self, mutate=None):
        self.calls = []
        self.worker = Mock()

        def rpc(request):
            self.calls.append(request)
            result = ({"opened": True} if request["operation"] == "open" else
                      {"closed": True} if request["operation"] == "close" else
                      {"inputs": [{"index": 0, "name": "Watch"}], "current_input": 0}
                      if request["operation"] == "read_inputs" else 0)
            reply = _reply(request, result)
            if mutate and request["operation"] not in ("open", "close"):
                mutate(reply)
            return _check_reply(reply, request)

        self.worker.rpc.side_effect = rpc

        enrollment = SimpleNamespace(python="python", storage="~/private-matter", timeout=90,
                                     sdk_path="/sdk", paa_path="/paa")
        dut = SimpleNamespace(name="amity-dut", identity=SimpleNamespace(device_id=DEVICE),
                              _runtime=RunConfig(timeout=60, enrollment_factory=lambda: enrollment))
        self.factory = Mock(return_value=self.worker)
        return MatterAccessory(dut, worker_factory=self.factory)

    def test_uses_saved_controller_without_supplying_any_credentials(self):
        with self.accessory() as accessory:
            self.assertEqual(accessory.read_inputs()["inputs"], [{"index": 0, "name": "Watch"}])
            self.assertEqual(accessory.select_input(1), 0)
            self.assertEqual(accessory.send_key(0xFE), 0)
            self.assertEqual(accessory.send_key(Key.SELECT), 0)
            self.assertEqual(accessory.playback("pause"), 0)
            self.assertEqual(accessory.set_mute(True), 0)
            self.assertEqual(accessory.set_power(False), 0)
            self.assertEqual(accessory.step_volume(-1), 0)
        operations = [request["operation"] for request in self.calls]
        self.assertEqual(operations, ["open", "read_inputs", "select_input", "send_key", "send_key", "playback", "mute", "power", "volume", "close"])
        for request in self.calls:
            self.assertNotIn("credentials", request)
            self.assertEqual(request["device_id"], DEVICE)
        self.assertEqual(len({request["request_id"] for request in self.calls}), len(self.calls))
        self.assertEqual(self.calls[6]["arguments"], {"on": False})
        self.worker.start.assert_called_once()
        self.worker.stop.assert_called_once()
        self.assertEqual(self.factory.call_args.args[1], 60)
        accessory.close()
        self.worker.stop.assert_called_once()
        with self.assertRaises(MatterError):
            accessory.send_key(0)

    def test_rejects_response_for_another_device_or_operation(self):
        for mutate in (lambda reply: reply.update(device_id="f" * 32),
                       lambda reply: reply.update(operation="power"),
                       lambda reply: reply.update(result=True),
                       lambda reply: reply.update(request_id="bad")):
            with self.subTest(mutate=mutate), self.assertRaises(MatterError):
                with self.accessory(mutate) as accessory:
                    accessory.send_key(0)
            self.assertEqual([request["operation"] for request in self.calls], ["open", "send_key"])
            self.worker.stop.assert_called_once()

    def test_validates_public_arguments_before_launching_controller(self):
        accessory = self.accessory()
        for call in (lambda: accessory.send_key(True), lambda: accessory.select_input(-1),
                     lambda: accessory.send_key(256), lambda: accessory.step_volume(0),
                     lambda: accessory.playback("rewind"), lambda: accessory.set_power(1)):
            with self.assertRaises(ValueError):
                call()
        self.assertEqual(self.calls, [])

    def test_context_requires_open_and_never_reopens_after_close_or_bad_open(self):
        accessory = self.accessory()
        with self.assertRaises(MatterError):
            accessory.read_inputs()
        self.worker.start.assert_not_called()
        for malformed in ({"opened": 1}, {"opened": True, "extra": 0}, {}, None):
            with self.subTest(malformed=malformed):
                accessory = self.accessory()
                self.worker.rpc.side_effect = None
                self.worker.rpc.return_value = malformed
                with self.assertRaises(MatterError):
                    accessory.__enter__()
                with self.assertRaises(MatterError):
                    accessory.__enter__()
                self.worker.start.assert_called_once()
                self.worker.stop.assert_called_once()
        accessory = self.accessory()
        with accessory:
            with self.assertRaises(MatterError):
                accessory.__enter__()
        with self.assertRaises(MatterError):
            accessory.__enter__()

    def test_bad_close_ack_poisoned_and_primary_error_preserved(self):
        accessory = self.accessory()
        accessory.__enter__()
        self.worker.rpc.side_effect = None
        self.worker.rpc.return_value = {"closed": 1}
        with self.assertRaises(MatterError):
            accessory.close()
        accessory.close()
        self.worker.stop.assert_called_once()
        accessory = self.accessory()
        with self.assertRaisesRegex(AssertionError, "original failure"):
            with accessory:
                self.worker.rpc.side_effect = RuntimeError("private SDK details")
                raise AssertionError("original failure")

    def test_worker_reuses_current_generation_and_private_platform(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            root.chmod(0o700)
            owner = root / DEVICE
            owner.mkdir(mode=0o700)
            storage = owner / "generation-test"
            storage.mkdir(mode=0o700)
            (storage / "controller.json").write_text("saved-fabric")
            (storage / "platform").mkdir(mode=0o700)
            (owner / "current").symlink_to(storage.name)
            sdk, paa = root / "sdk", root / "paa"
            sdk.mkdir()
            paa.mkdir()
            request = {"request_id": "f" * 32, "device_id": DEVICE,
                       "operation": "open", "arguments": {}}
            args = SimpleNamespace(storage=str(root), sdk_path=str(sdk), paa_path=str(paa), timeout=5)
            response = None
            native_input = tempfile.TemporaryFile()
            process = SimpleNamespace(stdin=native_input, poll=lambda: None)
            following = [{**request, "request_id": "e" * 32, "operation": "send_key", "arguments": {"key": 0}},
                         {**request, "request_id": "d" * 32, "operation": "close"}]

            def popen(command, **options):
                nonlocal response
                self.assertEqual(command[:6], ["sudo", "-n", "unshare", "--mount", "--propagation", "private"])
                self.assertEqual(command[command.index("--storage") + 1], str(storage))
                response = Path(command[command.index("--response") + 1])
                self.assertEqual(response.parent.parent, storage)
                self.assertTrue(options["start_new_session"])
                return process

            from amity_harness.matter import _write_message
            def write(stream, value, deadline, cancelled=None):
                _write_message(stream, value, deadline, cancelled)
                if stream is native_input:
                    result = {"opened": True} if value["operation"] == "open" else {"closed": True} if value["operation"] == "close" else 0
                    _store_reply(response, _reply(value, result))

            with tempfile.TemporaryFile() as incoming, tempfile.TemporaryFile() as outgoing:
                incoming.write(b"".join((json.dumps(row) + "\n").encode() for row in following))
                incoming.seek(0)
                with patch("amity_harness.matter._write_message", side_effect=write), \
                        patch("amity_harness.matter._stop_process_group") as stop:
                    _worker(request, args, popen=popen, input_stream=incoming, output_stream=outgoing)
                stop.assert_called_once_with(process)
                outgoing.seek(0)
                replies = [json.loads(line) for line in outgoing]
                self.assertEqual([row["operation"] for row in replies], ["open", "send_key", "close"])
                self.assertEqual(replies[-1]["result"], {"closed": True})
            self.assertEqual((storage / "controller.json").read_text(), "saved-fabric")
            self.assertEqual((owner / "current").readlink(), Path(storage.name))
            logs = list(storage.glob("accessory-*/controller.log"))
            self.assertEqual(len(logs), 1)
            self.assertFalse(response.exists())

    def test_worker_requires_existing_enrollment(self):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(storage=directory)
            with self.assertRaises(FileNotFoundError):
                _worker({"device_id": DEVICE, "operation": "open"}, args)

    def test_request_rejects_path_traversal_and_unsupported_operation(self):
        request = {"request_id": "f" * 32, "device_id": DEVICE,
                   "operation": "send_key", "arguments": {"key": 0}}
        self.assertEqual(_validate_request(request), request)
        for changes in ({"device_id": "../../other"}, {"operation": "commission"}, {"arguments": []}):
            with self.assertRaises(ValueError):
                _validate_request({**request, **changes})


class MatterPipeTests(unittest.TestCase):
    def worker(self, code, directory, *, timeout=1):
        path = Path(directory) / "commands.jsonl"
        process = subprocess.Popen([sys.executable, "-u", "-c", code, str(path)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, bufsize=0, start_new_session=True)
        self.addCleanup(process.wait, timeout=5)
        worker = _Worker(SimpleNamespace(), timeout)
        worker.process = process
        os.set_blocking(process.stdin.fileno(), False)
        os.set_blocking(process.stdout.fileno(), False)
        self.addCleanup(worker.stop)
        return worker, process, path

    def test_persistent_pipe_correlates_multiple_commands_without_replay(self):
        code = """import json,sys
for line in sys.stdin:
 request=json.loads(line)
 with open(sys.argv[1],'a') as log:log.write(line)
 reply={key:request[key] for key in ('request_id','device_id','operation')}
 reply['result']=0
 print(json.dumps(reply),flush=True)
"""
        with tempfile.TemporaryDirectory() as directory:
            worker, process, path = self.worker(code, directory)
            for index, operation in enumerate(("send_key", "read_inputs", "send_key")):
                request = {"request_id": f"{index:032x}", "device_id": DEVICE,
                           "operation": operation, "arguments": {}}
                self.assertEqual(worker.rpc(request), 0)
                self.assertIs(worker.process, process)
            self.assertEqual(len(path.read_text().splitlines()), 3)
            worker.stop()
            worker.stop()
            self.assertIsNotNone(process.poll())

    def test_transport_errors_poison_process_and_never_retry(self):
        base = """import json,sys,time
line=sys.stdin.readline()
with open(sys.argv[1],'a') as log:log.write(line)
request=json.loads(line)
reply={key:request[key] for key in ('request_id','device_id','operation')}
reply['result']=0
"""
        cases = ("reply['request_id']='wrong'; print(json.dumps(reply),flush=True)",
                 "reply['device_id']='f'*32; print(json.dumps(reply),flush=True)",
                 "reply['operation']='power'; print(json.dumps(reply),flush=True)",
                 "print('x'*17000,flush=True)", "print('invalid JSON',flush=True)",
                 "sys.stdout.write('{');sys.stdout.flush()", "pass", "time.sleep(10)")
        for ending in cases:
            with self.subTest(ending=ending), tempfile.TemporaryDirectory() as directory:
                worker, process, path = self.worker(base + ending, directory, timeout=.2)
                request = {"request_id": "a" * 32, "device_id": DEVICE,
                           "operation": "send_key", "arguments": {"key": 3}}
                with self.assertRaises(Exception):
                    worker.rpc(request)
                self.assertIsNone(worker.process)
                self.assertIsNotNone(process.poll())
                self.assertEqual(len(path.read_text().splitlines()), 1)
                with self.assertRaises(MatterError):
                    worker.rpc(request)
                self.assertEqual(len(path.read_text().splitlines()), 1)

    def test_deadline_includes_lock_wait_and_interruption_stops_worker(self):
        with tempfile.TemporaryDirectory() as directory:
            worker, process, path = self.worker("import time;time.sleep(10)", directory, timeout=.05)
            worker.lock.acquire()
            started = time.monotonic()
            try:
                with self.assertRaises(TimeoutError):
                    worker.rpc({})
            finally:
                worker.lock.release()
            self.assertLess(time.monotonic() - started, 1)
            self.assertIsNotNone(process.poll())
            self.assertFalse(path.exists())
        for interruption in (KeyboardInterrupt, SystemExit):
            with self.subTest(interruption=interruption), tempfile.TemporaryDirectory() as directory:
                worker, process, _ = self.worker("import time;time.sleep(10)", directory)
                with patch("amity_harness.matter._write_message", side_effect=interruption):
                    with self.assertRaises(interruption):
                        worker.rpc({})
                self.assertIsNone(worker.process)
                self.assertIsNotNone(process.poll())


class MatterNativeSessionTests(unittest.IsolatedAsyncioTestCase):
    async def session(self, requests, *, failure=None, shutdown_error=False):
        events, replies, calls = [], [], []
        loop = asyncio.get_running_loop()

        def shutdown(name):
            self.assertIs(asyncio.get_running_loop(), loop)
            events.append(name)
            if name == "controller" and shutdown_error:
                raise RuntimeError("native shutdown failed")

        controller = SimpleNamespace(Shutdown=lambda: shutdown("controller"))
        admin = SimpleNamespace(NewController=Mock(return_value=controller))
        manager = SimpleNamespace(activeCaList=[SimpleNamespace(adminList=[admin])],
                                  LoadAuthoritiesFromStorage=Mock(), Shutdown=lambda: shutdown("manager"))
        stack = SimpleNamespace(Shutdown=lambda: shutdown("stack"))
        stack_factory, manager_factory = Mock(return_value=stack), Mock(return_value=manager)
        read_fd, write_fd = os.pipe()
        iterator = iter(requests)

        def store(path, reply):
            replies.append(reply)
            if reply["operation"] == "close":
                self.assertEqual(events, ["controller", "manager", "stack"])
            else:
                upcoming = next(iterator, None)
                if upcoming is None:
                    os.close(write_fd)
                else:
                    # This callback must run while native waits for a request;
                    # a blocking event-loop read would deadlock this test.
                    loop.call_later(.01, os.write, write_fd, (json.dumps(upcoming) + "\n").encode())

        async def operate(actual, clusters, request, timeout):
            self.assertIs(actual, controller)
            self.assertIs(asyncio.get_running_loop(), loop)
            calls.append(request)
            if failure:
                raise failure
            return 0

        args = SimpleNamespace(storage="/private/fabric", paa_path="/private/paa", timeout=.2,
                               response="/private/operation/result.json")
        opened = {"request_id": "0" * 32, "device_id": DEVICE, "operation": "open", "arguments": {}}
        try:
            with os.fdopen(read_fd, "rb", buffering=0) as incoming, \
                    patch("amity_harness.matter._store_reply", side_effect=store), \
                    patch("amity_harness.matter._operate", side_effect=operate):
                await _native_session(opened, args, stack_factory, manager_factory, object(), input_stream=incoming)
        finally:
            try:
                os.close(write_fd)
            except OSError:
                pass
            self.assertEqual(events, ["controller", "manager", "stack"])
            stack_factory.assert_called_once_with("/private/fabric/controller.json")
            manager_factory.assert_called_once_with(stack)
            manager.LoadAuthoritiesFromStorage.assert_called_once()
            admin.NewController.assert_called_once_with(nodeId=112233, paaTrustStorePath="/private/paa")
        return replies, calls

    async def test_native_controller_and_event_loop_survive_all_commands_until_close(self):
        requests = [{"request_id": f"{index:032x}", "device_id": DEVICE,
                     "operation": operation, "arguments": arguments}
                    for index, (operation, arguments) in enumerate((
                        ("send_key", {"key": 3}), ("volume", {"steps": 1}), ("close", {})), start=1)]
        replies, calls = await self.session(requests)
        self.assertEqual([request["operation"] for request in calls], ["send_key", "volume"])
        self.assertEqual([reply["operation"] for reply in replies], ["open", "send_key", "volume", "close"])
        self.assertEqual(replies[-1]["result"], {"closed": True})

    async def test_native_timeout_wrong_identity_and_eof_all_clean_up_without_replay(self):
        command = {"request_id": "1" * 32, "device_id": DEVICE,
                   "operation": "send_key", "arguments": {"key": 3}}
        with self.assertRaises(TimeoutError):
            await self.session([command], failure=TimeoutError("uncertain result"))
        for changes in ({"device_id": "f" * 32}, {"operation": "open", "arguments": {}},
                        {"arguments": {"key": True}}):
            with self.subTest(changes=changes), self.assertRaises((MatterError, ValueError)):
                await self.session([{**command, **changes}])
        replies, calls = await self.session([])
        self.assertEqual([row["operation"] for row in replies], ["open"])
        self.assertEqual(calls, [])

    async def test_native_close_failure_still_shuts_down_all_owners(self):
        request = {"request_id": "1" * 32, "device_id": DEVICE, "operation": "close", "arguments": {}}
        with self.assertRaisesRegex(RuntimeError, "native shutdown failed"):
            await self.session([request], shutdown_error=True)


class MatterTranslationTests(unittest.IsolatedAsyncioTestCase):
    async def test_commands_use_accessory_endpoints_and_preserve_unsupported_keys(self):
        class InteractionError(Exception):
            def __init__(self, status):
                self.status = status

        modules = {"chip": SimpleNamespace(),
                   "chip.interaction_model": SimpleNamespace(InteractionModelError=InteractionError)}
        clusters = SimpleNamespace(**{name: SimpleNamespace(Commands=SimpleNamespace())
                                     for name in ("MediaInput", "KeypadInput", "MediaPlayback", "OnOff", "LevelControl")})
        for cluster, names in (("MediaInput", ("SelectInput",)), ("KeypadInput", ("SendKey",)),
                               ("MediaPlayback", ("Play", "Pause", "Stop", "Previous", "Next")),
                               ("OnOff", ("On", "Off")), ("LevelControl", ("Step",))):
            for name in names:
                setattr(getattr(clusters, cluster).Commands, name,
                        Mock(side_effect=lambda _name=name, **arguments: (_name, arguments)))
        controller = SimpleNamespace(SendCommand=AsyncMock(return_value=SimpleNamespace(status=0)))
        cases = (("select_input", {"index": 1}, 1, ("SelectInput", {"index": 1})),
                 ("send_key", {"key": 0xFE}, 1, ("SendKey", {"keyCode": 0xFE})),
                 ("playback", {"command": "previous"}, 1, ("Previous", {})),
                 ("power", {"on": True}, 1, ("On", {})),
                 ("mute", {"on": False}, 2, ("Off", {})),
                 ("volume", {"steps": -2}, 2, ("Step", {"stepMode": 1, "stepSize": 2,
                      "transitionTime": 0, "optionsMask": 0, "optionsOverride": 0})))
        with patch.dict(sys.modules, modules):
            for operation, arguments, endpoint, command in cases:
                with self.subTest(operation=operation):
                    result = await _operate(controller, clusters, {"operation": operation, "arguments": arguments}, 1)
                    self.assertEqual(result, 0)
                    controller.SendCommand.assert_awaited_with(1, endpoint, command)
            controller.SendCommand.return_value = None
            self.assertEqual(await _operate(controller, clusters,
                {"operation": "select_input", "arguments": {"index": 1}}, 1), 0)
            for response in (None, object(), SimpleNamespace(status="0"), SimpleNamespace(status=True)):
                controller.SendCommand.return_value = response
                with self.assertRaises(MatterError):
                    await _operate(controller, clusters,
                        {"operation": "send_key", "arguments": {"key": 0}}, 1)
            controller.SendCommand.side_effect = InteractionError(0x85)
            self.assertEqual(await _operate(controller, clusters,
                {"operation": "select_input", "arguments": {"index": 255}}, 1), 0x85)

    async def test_input_list_retains_saved_slot_numbers(self):
        attributes = SimpleNamespace(InputList=object(), CurrentInput=object())
        cluster = type("MediaInput", (), {"Attributes": attributes})
        values = {attributes.InputList: [SimpleNamespace(index=0, name="TV"), SimpleNamespace(index=2, name="Music")],
                  attributes.CurrentInput: 2}
        controller = SimpleNamespace(ReadAttribute=AsyncMock(return_value={1: {cluster: values}}))
        result = await _operate(controller, SimpleNamespace(MediaInput=cluster),
                                {"operation": "read_inputs", "arguments": {}}, 1)
        self.assertEqual(result, {"inputs": [{"index": 0, "name": "TV"}, {"index": 2, "name": "Music"}],
                                  "current_input": 2})


if __name__ == "__main__":
    unittest.main()
