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
import select
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from uuid import uuid4

from .enrollment import (MatterEnrollment, _CLEANUP_ALLOWANCE, _MAX_REQUEST,
                         _signal_group, _stop_process_group, _worker_signals)
from .trace import event


class MatterError(RuntimeError):
    """Native details are retained only in the private Pi controller log."""


def _remaining(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("Matter accessory deadline expired")
    return remaining


def _check_cancelled(cancelled):
    if cancelled is not None and cancelled():
        raise MatterError("Matter accessory worker interrupted")


def _write_message(stream, value, deadline, cancelled=None):
    data = memoryview((json.dumps(value) + "\n").encode())
    if len(data) > _MAX_REQUEST:
        raise MatterError("Matter accessory message is too large")
    while data:
        _check_cancelled(cancelled)
        remaining = _remaining(deadline)
        _, ready, _ = select.select([], [stream], [], min(remaining, .05) if cancelled else remaining)
        if ready:
            data = data[os.write(stream.fileno(), data):]


def _read_message(stream, buffer, deadline=None, cancelled=None):
    while b"\n" not in buffer:
        _check_cancelled(cancelled)
        remaining = None if deadline is None else _remaining(deadline)
        wait = min(remaining, .05) if remaining is not None else .05
        ready, _, _ = select.select([stream], [], [], wait if cancelled else remaining)
        if ready:
            chunk = os.read(stream.fileno(), _MAX_REQUEST + 1)
            if not chunk:
                if buffer:
                    raise MatterError("Matter accessory message was truncated")
                return None
            buffer.extend(chunk)
            if len(buffer) > _MAX_REQUEST:
                raise MatterError("Matter accessory message is too large")
    _check_cancelled(cancelled)
    line, _, rest = buffer.partition(b"\n")
    buffer[:] = rest
    return json.loads(line)


def _reply(request, result):
    return {**{key: request[key] for key in ("request_id", "device_id", "operation")}, "result": result}


def _check_reply(reply, request):
    if (not isinstance(reply, dict) or set(reply) != {"request_id", "device_id", "operation", "result"}
            or any(reply[key] != request[key] for key in ("request_id", "device_id", "operation"))):
        raise MatterError("Matter worker did not confirm the requested command")
    return reply["result"]


class _Worker:
    """One supervisor owns the isolated native controller for this context."""

    def __init__(self, enrollment, timeout):
        self.enrollment, self.timeout = enrollment, timeout
        self.process = None
        self.buffer = bytearray()
        self.lock = threading.Lock()

    def start(self):
        enrollment = self.enrollment
        command = [enrollment.python, "-u", "-m", "amity_harness.matter", "--worker",
                   "--storage", enrollment.storage, "--timeout", str(self.timeout)]
        for option, value in (("--sdk-path", enrollment.sdk_path), ("--paa-path", enrollment.paa_path)):
            if value is not None:
                command += [option, value]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, bufsize=0, start_new_session=True,
                                        env={**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])})
        os.set_blocking(self.process.stdin.fileno(), False)
        os.set_blocking(self.process.stdout.fileno(), False)

    def rpc(self, request):
        deadline = time.monotonic() + self.timeout
        if not self.lock.acquire(timeout=_remaining(deadline)):
            self.stop()
            raise TimeoutError("Matter accessory worker is busy")
        try:
            if self.process is None:
                raise MatterError("Matter accessory context is closed")
            _write_message(self.process.stdin, request, deadline)
            reply = _read_message(self.process.stdout, self.buffer, deadline)
            return _check_reply(reply, request)
        except BaseException:
            # The command may already have taken effect. Never resend it or
            # reuse a native controller with late callbacks after cancellation.
            self.stop()
            raise
        finally:
            self.lock.release()

    def stop(self):
        previous = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGHUP, signal.SIGTERM})
        process, self.process = self.process, None
        try:
            if process is None:
                return
            if process.poll() is None:
                _signal_group(process.pid, signal.SIGTERM)
            try:
                # The supervisor must stop its separate native process group
                # before releasing the fabric lock. Allow its whole cleanup.
                process.wait(timeout=_CLEANUP_ALLOWANCE)
            except subprocess.TimeoutExpired:
                _stop_process_group(process)
        finally:
            try:
                if process is not None:
                    for stream in (process.stdin, process.stdout):
                        if stream is not None:
                            stream.close()
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def _byte(value, name):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 255:
        raise ValueError(name + " must be an integer from 0 to 255")
    return int(value)


class MatterAccessory:
    """Use the controller fabric created by the runner's main enrollment."""

    def __init__(self, dut, *, worker_factory=None):
        if dut.name != dut._runtime.main_dut:
            raise ValueError("Matter accessory control requires the selected main DUT")
        self.dut = dut
        self.enrollment = (dut._runtime.enrollment_factory or MatterEnrollment)()
        self.worker = (worker_factory or _Worker)(self.enrollment,
                    min(self.enrollment.timeout, dut._runtime.timeout))
        self._entered = self._closed = False

    def __enter__(self):
        if self._entered or self._closed:
            raise MatterError("Matter accessory context cannot be reopened")
        try:
            self.worker.start()
            opened = self.worker.rpc(self._command("open"))
            if not isinstance(opened, dict) or set(opened) != {"opened"} or opened["opened"] is not True:
                raise MatterError("Matter controller did not open its saved fabric")
            self._entered = True
        except (KeyboardInterrupt, SystemExit):
            self._closed = True
            self.worker.stop()
            raise
        except Exception:
            self._closed = True
            self.worker.stop()
            raise MatterError("Matter accessory could not open; inspect its private Pi controller log") from None
        return self

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except BaseException:
            if exc_type is None:
                raise
            event("matter.cleanup.failed", dut=self.dut.name)
        return False

    def _command(self, operation, **arguments):
        return {"request_id": uuid4().hex, "device_id": self.dut.identity.device_id,
                "operation": operation, "arguments": arguments}

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            if self._entered:
                closed = self.worker.rpc(self._command("close"))
                if not isinstance(closed, dict) or set(closed) != {"closed"} or closed["closed"] is not True:
                    raise MatterError("Matter controller did not close")
        finally:
            self.worker.stop()

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
        if not self._entered or self._closed:
            raise MatterError("Matter accessory has no active context")
        request = self._command(operation, **arguments)
        event("matter.command.start", dut=self.dut.name, operation=operation, arguments=arguments)
        try:
            result = self.worker.rpc(request)
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
            self._closed = True
            self.worker.stop()
            raise
        except Exception:
            self._closed = True
            self.worker.stop()
            raise MatterError("Matter accessory operation failed; inspect its private Pi controller log") from None
        event("matter.command.complete", dut=self.dut.name, operation=operation, result=result)
        return result


def _validate_request(value):
    if (not isinstance(value, dict) or set(value) != {"request_id", "device_id", "operation", "arguments"}
            or any(not isinstance(value[field], str) or re.fullmatch(r"[0-9a-f]{32}", value[field]) is None
                   for field in ("request_id", "device_id"))
            or value["operation"] not in ("open", "close", "read_inputs", "select_input", "send_key", "playback", "power", "mute", "volume")
            or not isinstance(value["arguments"], dict)):
        raise ValueError("Invalid Matter accessory request")
    operation, arguments = value["operation"], value["arguments"]
    fields = {"select_input": {"index"}, "send_key": {"key"}, "playback": {"command"},
              "power": {"on"}, "mute": {"on"}, "volume": {"steps"}}
    if set(arguments) != fields.get(operation, set()):
        raise ValueError("Invalid Matter accessory arguments")
    if operation == "select_input":
        _byte(arguments["index"], "Input index")
    elif operation == "send_key":
        _byte(arguments["key"], "Matter key")
    elif operation == "playback" and arguments["command"] not in ("play", "pause", "stop", "previous", "next"):
        raise ValueError("Invalid Matter playback command")
    elif operation in ("power", "mute") and type(arguments["on"]) is not bool:
        raise ValueError("Invalid Matter power value")
    elif operation == "volume" and (type(arguments["steps"]) is not int or not 1 <= abs(arguments["steps"]) <= 255):
        raise ValueError("Invalid Matter volume steps")
    return value


def _worker(request, args, *, popen=None, input_stream=None, output_stream=None, cancelled=None):
    if request["operation"] != "open":
        raise MatterError("Matter accessory must open before issuing commands")
    input_stream = sys.stdin.buffer if input_stream is None else input_stream
    output_stream = sys.stdout.buffer if output_stream is None else output_stream
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
        buffer = bytearray()
        closed = None
        with (operation / "controller.log").open("x") as log:
            process = (popen or subprocess.Popen)(command, stdin=subprocess.PIPE, stdout=log,
                                                  stderr=subprocess.STDOUT, start_new_session=True,
                                                  bufsize=0)
            try:
                os.set_blocking(process.stdin.fileno(), False)
                while True:
                    _check_cancelled(cancelled)
                    if request["device_id"] != owner.name:
                        raise MatterError("Matter accessory changed its context identity")
                    deadline = time.monotonic() + args.timeout
                    _write_message(process.stdin, request, deadline, cancelled)
                    while not response_path.exists():
                        _check_cancelled(cancelled)
                        if process.poll() is not None:
                            raise MatterError("Native Matter controller ended unexpectedly")
                        time.sleep(min(.02, _remaining(deadline)))
                    if response_path.stat().st_size > _MAX_REQUEST:
                        raise MatterError("Native Matter reply is too large")
                    reply = json.loads(response_path.read_text(encoding="utf-8"))
                    response_path.unlink()
                    _check_reply(reply, request)
                    if request["operation"] == "close":
                        closed = reply
                        break
                    _write_message(output_stream, reply, deadline, cancelled)
                    value = _read_message(input_stream, buffer, cancelled=cancelled)
                    if value is None:
                        break
                    request = _validate_request(value)
                    if request["operation"] == "open":
                        raise MatterError("Matter accessory context is already open")
            finally:
                # Do not release accessory.lock until all native descendants
                # have stopped, including cancellation and parent-pipe EOF.
                try:
                    _stop_process_group(process)
                finally:
                    process.stdin.close()
    if closed is not None:
        _write_message(output_stream, closed, time.monotonic() + args.timeout, cancelled)


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


def _store_reply(path, reply):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(reply), encoding="utf-8")
    temporary.replace(path)


async def _native_session(request, args, stack_factory, manager_factory, clusters, *, input_stream=None):
    """Keep the controller and its callback event loop alive until context close."""
    input_stream = sys.stdin.buffer if input_stream is None else input_stream
    storage = Path(args.storage)
    stack = manager = controller = None
    closed = None
    device_id = request["device_id"]
    try:
        if request["operation"] != "open":
            raise MatterError("Matter accessory must open its saved controller first")
        stack = stack_factory(str(storage / "controller.json"))
        manager = manager_factory(stack)
        manager.LoadAuthoritiesFromStorage()
        if len(manager.activeCaList) != 1 or len(manager.activeCaList[0].adminList) != 1:
            raise MatterError("The saved commissioning controller has no unique fabric")
        controller = manager.activeCaList[0].adminList[0].NewController(
            nodeId=112233, paaTrustStorePath=str(args.paa_path))
        _store_reply(Path(args.response), _reply(request, {"opened": True}))
        buffer = bytearray()
        while True:
            # Native callbacks schedule work on this loop, even while idle.
            value = await asyncio.to_thread(_read_message, input_stream, buffer)
            if value is None:
                break
            request = _validate_request(value)
            if request["device_id"] != device_id or request["operation"] == "open":
                raise MatterError("Matter accessory changed its context identity")
            if request["operation"] == "close":
                closed = request
                break
            result = await _operate(controller, clusters, request, args.timeout)
            _store_reply(Path(args.response), _reply(request, result))
    finally:
        # Shutdown may finish native callbacks. Keep their loop alive until the
        # CHIP thread has stopped, and attempt every cleanup if one fails.
        try:
            if controller is not None:
                controller.Shutdown()
        finally:
            try:
                if manager is not None:
                    manager.Shutdown()
            finally:
                if stack is not None:
                    stack.Shutdown()
    if closed is not None:
        _store_reply(Path(args.response), _reply(closed, {"closed": True}))


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

    chip.native.Init()
    asyncio.run(_native_session(request, args, ChipStack, CertificateAuthorityManager, Clusters))


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
        if not args.native_worker:
            os.set_blocking(sys.stdout.fileno(), False)
        value = _read_message(sys.stdin.buffer, bytearray())
        request = _validate_request(value)
        if args.native_worker:
            _native_worker(request, args)
        else:
            with _worker_signals():
                # Equal command deadlines can signal the supervisor as it
                # enters native cleanup. A signal handler must never raise
                # through that finally block and release the fabric early.
                interrupted = [False]

                def cancel(signum, frame):
                    interrupted[0] = True

                for signum in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM):
                    signal.signal(signum, cancel)
                _worker(request, args, cancelled=lambda: interrupted[0])
        return 0
    except Exception as error:
        if args.native_worker:
            traceback.print_exc()
        print(json.dumps({"error_type": type(error).__name__}), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
