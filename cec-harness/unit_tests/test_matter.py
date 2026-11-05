"""Saved-fabric isolation and real accessory command translation."""

import asyncio
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.keys import Key
from amity_harness.config import RunConfig
from amity_harness.matter import MatterAccessory, MatterError, _operate, _request, _worker


DEVICE = "0123456789abcdef0123456789abcdef"


class MatterAccessoryTests(unittest.TestCase):
    def accessory(self, mutate=None):
        self.calls = []

        def run(command, **options):
            request = json.loads(options["input"])
            self.calls.append((command, options, request))
            result = ({"inputs": [{"index": 0, "name": "Watch"}], "current_input": 0}
                      if request["operation"] == "read_inputs" else 0)
            reply = {key: request[key] for key in ("request_id", "device_id", "operation")}
            reply["result"] = result
            if mutate:
                mutate(reply)
            return subprocess.CompletedProcess(command, 0, json.dumps(reply), "private SDK text")

        enrollment = SimpleNamespace(python="python", storage="~/private-matter", timeout=90,
                                     sdk_path="/sdk", paa_path="/paa", runner=run)
        dut = SimpleNamespace(name="amity-dut", identity=SimpleNamespace(device_id=DEVICE),
                              _runtime=RunConfig(timeout=60, enrollment_factory=lambda: enrollment))
        return MatterAccessory(dut)

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
        operations = [request["operation"] for _, _, request in self.calls]
        self.assertEqual(operations, ["read_inputs", "select_input", "send_key", "send_key", "playback", "mute", "power", "volume"])
        for command, options, request in self.calls:
            self.assertNotIn("credentials", request)
            self.assertIn("amity_harness.matter", command)
            self.assertIn("~/private-matter", command)
            self.assertEqual(options["timeout"], 70)
        self.assertEqual(self.calls[5][2]["arguments"], {"on": False})

    def test_rejects_response_for_another_device_or_operation(self):
        for mutate in (lambda reply: reply.update(device_id="f" * 32),
                       lambda reply: reply.update(operation="power"),
                       lambda reply: reply.update(result=True),
                       lambda reply: reply.update(request_id="bad")):
            with self.subTest(mutate=mutate), self.assertRaises(MatterError):
                self.accessory(mutate).send_key(0)

    def test_validates_public_arguments_before_launching_controller(self):
        accessory = self.accessory()
        for call in (lambda: accessory.send_key(True), lambda: accessory.select_input(-1),
                     lambda: accessory.send_key(256), lambda: accessory.step_volume(0),
                     lambda: accessory.playback("rewind"), lambda: accessory.set_power(1)):
            with self.assertRaises(ValueError):
                call()
        self.assertEqual(self.calls, [])

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
                       "operation": "send_key", "arguments": {"key": 0}}
            reply = {**request, "result": 0}
            del reply["arguments"]
            args = SimpleNamespace(storage=str(root), sdk_path=str(sdk), paa_path=str(paa), timeout=5)

            def run(command, **options):
                self.assertEqual(command[:6], ["sudo", "-n", "unshare", "--mount", "--propagation", "private"])
                self.assertEqual(command[command.index("--storage") + 1], str(storage))
                response = Path(command[command.index("--response") + 1])
                self.assertEqual(response.parent.parent, storage)
                response.write_text(json.dumps(reply))
                self.assertEqual(json.loads(options["input"]), request)
                self.assertEqual(options["timeout"], 5)
                return subprocess.CompletedProcess(command, 0)

            self.assertEqual(_worker(request, args, runner=run), reply)
            self.assertEqual((storage / "controller.json").read_text(), "saved-fabric")
            self.assertEqual((owner / "current").readlink(), Path(storage.name))
            logs = list(storage.glob("accessory-*/controller.log"))
            self.assertEqual(len(logs), 1)

    def test_worker_requires_existing_enrollment(self):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(storage=directory)
            with self.assertRaises(FileNotFoundError):
                _worker({"device_id": DEVICE}, args)

    def test_request_rejects_path_traversal_and_unsupported_operation(self):
        request = {"request_id": "f" * 32, "device_id": DEVICE,
                   "operation": "send_key", "arguments": {"key": 0}}
        self.assertEqual(_request(io.StringIO(json.dumps(request))), request)
        for changes in ({"device_id": "../../other"}, {"operation": "commission"}, {"arguments": []}):
            with self.assertRaises(ValueError):
                _request(io.StringIO(json.dumps({**request, **changes})))


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
