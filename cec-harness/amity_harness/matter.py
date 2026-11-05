"""Real accessory commands on the main's existing Matter commissioning fabric.

Configuration and DUT observations remain on authenticated HTTPS. This helper
reopens the Pi controller's saved fabric solely to exercise the shipping Matter
accessory, with native logs and controller material kept in private Pi storage.
"""

import argparse
import asyncio
import fcntl
import json
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys
import tempfile
import traceback
from uuid import uuid4

from .enrollment import (MatterEnrollment, _CLEANUP_ALLOWANCE, _MAX_REQUEST,
                         _run_process_group, _worker_signals)
from .trace import event


class MatterError(RuntimeError):
    """Native details are retained only in the private Pi controller log."""


def _byte(value, name):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 255:
        raise ValueError(name + " must be an integer from 0 to 255")
    return int(value)


class MatterAccessory:
    """Use the controller fabric created by the runner's main enrollment."""

    def __init__(self, dut):
        if dut.name != dut._runtime.main_dut:
            raise ValueError("Matter accessory control requires the selected main DUT")
        self.dut = dut
        self.enrollment = (dut._runtime.enrollment_factory or MatterEnrollment)()

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        return False

    def read_inputs(self):
        return self._operation("read_inputs")

    def select_input(self, index):
        return self._operation("select_input", index=_byte(index, "Input index"))

    def send_key(self, key):
        return self._operation("send_key", key=_byte(key, "Matter key"))

    def playback(self, command):
        if command not in ("play", "pause", "stop", "previous", "next"):
            raise ValueError("Unsupported playback command")
        return self._operation("playback", command=command)

    def set_power(self, on):
        if type(on) is not bool:
            raise ValueError("Power must be a boolean")
        return self._operation("power", on=on)

    def set_mute(self, muted):
        if type(muted) is not bool:
            raise ValueError("Mute must be a boolean")
        return self._operation("mute", on=not muted)

    def step_volume(self, steps):
        if type(steps) is not int or not 1 <= abs(steps) <= 255:
            raise ValueError("Volume steps must be a nonzero integer from -255 to 255")
        return self._operation("volume", steps=steps)

    def _operation(self, operation, **arguments):
        enrollment = self.enrollment
        timeout = min(enrollment.timeout, self.dut._runtime.timeout)
        request = {"request_id": uuid4().hex, "device_id": self.dut.identity.device_id,
                   "operation": operation, "arguments": arguments}
        command = [enrollment.python, "-u", "-m", "amity_harness.matter", "--worker",
                   "--storage", enrollment.storage, "--timeout", str(timeout)]
        for option, value in (("--sdk-path", enrollment.sdk_path), ("--paa-path", enrollment.paa_path)):
            if value is not None:
                command += [option, value]
        environment = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])}
        event("matter.command.start", dut=self.dut.name, operation=operation, arguments=arguments)
        try:
            response = enrollment.runner(command, input=json.dumps(request), text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         timeout=timeout + _CLEANUP_ALLOWANCE, env=environment, check=False)
            if response.returncode != 0 or len(response.stdout) > _MAX_REQUEST:
                raise MatterError("Matter accessory worker failed")
            reply = json.loads(response.stdout)
            if (not isinstance(reply, dict) or reply.get("request_id") != request["request_id"]
                    or reply.get("device_id") != request["device_id"]
                    or reply.get("operation") != operation or "result" not in reply):
                raise MatterError("Matter worker did not confirm the requested command")
            result = reply["result"]
            if operation == "read_inputs":
                if (not isinstance(result, dict) or set(result) != {"inputs", "current_input"}
                        or type(result["current_input"]) is not int or not 0 <= result["current_input"] <= 255
                        or not isinstance(result["inputs"], list)
                        or any(not isinstance(row, dict) or set(row) != {"index", "name"}
                               or type(row["index"]) is not int or not 0 <= row["index"] <= 255
                               or not isinstance(row["name"], str) for row in result["inputs"])):
                    raise MatterError("Matter worker returned invalid input attributes")
            elif type(result) is not int or not 0 <= result <= 255:
                raise MatterError("Matter worker returned no command status")
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception:
            raise MatterError("Matter accessory operation failed; inspect its private Pi controller log") from None
        event("matter.command.complete", dut=self.dut.name, operation=operation, result=result)
        return result


def _request(stream):
    value = json.loads(stream.read(_MAX_REQUEST + 1))
    if (not isinstance(value, dict) or set(value) != {"request_id", "device_id", "operation", "arguments"}
            or any(not isinstance(value[field], str) or re.fullmatch(r"[0-9a-f]{32}", value[field]) is None
                   for field in ("request_id", "device_id"))
            or value["operation"] not in ("read_inputs", "select_input", "send_key", "playback", "power", "mute", "volume")
            or not isinstance(value["arguments"], dict)):
        raise ValueError("Invalid Matter accessory request")
    return value


def _worker(request, args, *, runner=None):
    root = Path(args.storage).expanduser().resolve()
    owner = root / request["device_id"]
    storage = (owner / "current").resolve(strict=True)
    if storage.parent != owner or not (storage / "controller.json").is_file():
        raise MatterError("No successful Matter enrollment is saved for this DUT")
    if any(stat.S_IMODE(path.stat().st_mode) & 0o077 for path in (root, owner, storage)):
        raise MatterError("Matter controller storage must be private")
    project = Path(__file__).resolve().parents[2]
    sdk = Path(args.sdk_path).expanduser().resolve() if args.sdk_path else project / ".matter-python"
    paa = Path(args.paa_path).expanduser().resolve() if args.paa_path else project / "paa-root-certs"
    if not sdk.is_dir() or not paa.is_dir():
        raise MatterError("The Pi Matter SDK and trust store must be installed")
    # Loading a controller can update its stored session state. Serialize access
    # to this fabric, keeping the lock until cancellation has stopped descendants.
    with (storage / "accessory.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        operation = Path(tempfile.mkdtemp(prefix="accessory-", dir=storage))
        response_path = operation / "result.json"
        command = ["sudo", "-n", "unshare", "--mount", "--propagation", "private", "env",
                   "PYTHONPATH=" + str(Path(__file__).resolve().parents[1]), sys.executable,
                   "-u", "-m", "amity_harness.matter", "--native-worker", "--storage", str(storage),
                   "--sdk-path", str(sdk), "--paa-path", str(paa), "--timeout", str(args.timeout),
                   "--response", str(response_path)]
        with (operation / "controller.log").open("w") as log:
            result = (runner or _run_process_group)(command, input=json.dumps(request), text=True,
                                                   stdout=log, stderr=subprocess.STDOUT,
                                                   check=False, timeout=args.timeout)
        if result.returncode != 0 or not response_path.is_file() or response_path.stat().st_size > _MAX_REQUEST:
            raise MatterError("Native Matter command failed; inspect its private controller log")
        return json.loads(response_path.read_text(encoding="utf-8"))


async def _operate(controller, clusters, request, timeout):
    operation, arguments = request["operation"], request["arguments"]
    if operation == "read_inputs":
        cluster = clusters.MediaInput
        response = await asyncio.wait_for(controller.ReadAttribute(1, [
            (1, cluster.Attributes.InputList), (1, cluster.Attributes.CurrentInput)]), timeout)
        values = response[1][cluster]
        return {"inputs": [{"index": int(row.index), "name": row.name}
                           for row in values[cluster.Attributes.InputList]],
                "current_input": int(values[cluster.Attributes.CurrentInput])}
    endpoint = 1
    if operation == "select_input":
        command = clusters.MediaInput.Commands.SelectInput(index=arguments["index"])
    elif operation == "send_key":
        command = clusters.KeypadInput.Commands.SendKey(keyCode=arguments["key"])
    elif operation == "playback":
        names = {"play": "Play", "pause": "Pause", "stop": "Stop", "previous": "Previous", "next": "Next"}
        command = getattr(clusters.MediaPlayback.Commands, names[arguments["command"]])()
    elif operation in ("power", "mute"):
        endpoint = 1 if operation == "power" else 2
        command = (clusters.OnOff.Commands.On if arguments["on"] else clusters.OnOff.Commands.Off)()
    elif operation == "volume":
        endpoint = 2
        steps = arguments["steps"]
        command = clusters.LevelControl.Commands.Step(stepMode=0 if steps > 0 else 1,
                    stepSize=abs(steps), transitionTime=0, optionsMask=0, optionsOverride=0)
    else:
        raise ValueError("Unsupported accessory command")
    # Global Interaction Model failures are returned by the SDK as exceptions;
    # cluster-specific SendKey/Playback responses carry their own status.
    from chip.interaction_model import InteractionModelError
    try:
        response = await asyncio.wait_for(controller.SendCommand(1, endpoint, command), timeout)
    except InteractionModelError as error:
        return int(error.status)
    if response is None:
        if operation in ("send_key", "playback"):
            raise MatterError("Matter command omitted its required cluster response")
        return 0
    status = getattr(response, "status", None)
    if isinstance(status, bool) or not isinstance(status, int) or not 0 <= status <= 255:
        raise MatterError("Matter command returned an invalid status response")
    return int(status)


def _native_worker(request, args):
    storage = Path(args.storage)
    subprocess.run(["mount", "--bind", str(storage / "platform"), "/tmp"], check=True)
    if os.geteuid() == 0 and "SUDO_UID" in os.environ and "SUDO_GID" in os.environ:
        uid, gid = int(os.environ["SUDO_UID"]), int(os.environ["SUDO_GID"])
        os.initgroups(pwd.getpwuid(uid).pw_name, gid)
        os.setgid(gid)
        os.setuid(uid)
    sys.path.insert(0, str(Path(args.sdk_path)))
    import chip.native
    import chip.clusters as Clusters
    from chip.ChipStack import ChipStack
    from chip.CertificateAuthority import CertificateAuthorityManager

    stack = manager = controller = None
    try:
        chip.native.Init()
        stack = ChipStack(str(storage / "controller.json"))
        manager = CertificateAuthorityManager(stack)
        manager.LoadAuthoritiesFromStorage()
        if len(manager.activeCaList) != 1 or len(manager.activeCaList[0].adminList) != 1:
            raise MatterError("The saved commissioning controller has no unique fabric")
        controller = manager.activeCaList[0].adminList[0].NewController(
            nodeId=112233, paaTrustStorePath=str(args.paa_path))
        result = asyncio.run(_operate(controller, Clusters, request, args.timeout))
        reply = {key: request[key] for key in ("request_id", "device_id", "operation")}
        reply["result"] = result
        Path(args.response).write_text(json.dumps(reply), encoding="utf-8")
    finally:
        if controller is not None:
            controller.Shutdown()
        if manager is not None:
            manager.Shutdown()
        if stack is not None:
            stack.Shutdown()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--worker", action="store_true")
    modes.add_argument("--native-worker", action="store_true")
    parser.add_argument("--storage", required=True)
    parser.add_argument("--sdk-path")
    parser.add_argument("--paa-path")
    parser.add_argument("--response")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        from .dut import _positive_seconds
        _positive_seconds(args.timeout, "Matter accessory timeout")
        request = _request(sys.stdin)
        if args.native_worker:
            _native_worker(request, args)
        else:
            with _worker_signals():
                print(json.dumps(_worker(request, args)), flush=True)
        return 0
    except Exception as error:
        if args.native_worker:
            traceback.print_exc()
        print(json.dumps({"error_type": type(error).__name__}), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
