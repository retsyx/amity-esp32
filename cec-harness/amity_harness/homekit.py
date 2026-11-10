"""Own an aiohomekit IP pairing through the shipping HomeKit setup UI.

The persistent worker keeps its controller credentials and native diagnostics
private. Only selected characteristic values and HAP statuses cross its IPC.
"""

import argparse
import asyncio
from contextlib import AsyncExitStack, redirect_stdout
import ipaddress
import json
import logging
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from urllib.parse import urlsplit

from .dut import _positive_seconds
from .enrollment import _stop_process_group
from .homekit_srp import pair_setup_srp
from .trace import event


_MAX_MESSAGE = 65536
_BASE_UUID = "-0000-1000-8000-0026BB765291"


class HomeKitError(RuntimeError):
    """Controller diagnostics remain in its private HomeKit worker log."""


def _integer(value, maximum, description):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError(description + " is outside its unsigned characteristic range")
    return int(value)


def _uuid(value):
    value = value.upper()
    if value.endswith(_BASE_UUID):
        value = value[:-len(_BASE_UUID)]
    return f"{int(value, 16):X}"


class _Layout:
    """Resolve the advertised service graph without assuming any AID or IID."""

    def __init__(self, accessories, serial):
        candidates = [accessory for accessory in accessories
                      if any(_uuid(service["type"]) == "D8" for service in accessory["services"])]
        if len(candidates) != 1:
            raise HomeKitError("The accessory has no unique television")
        accessory, = candidates
        aid = accessory["aid"]

        def services(kind):
            return [service for service in accessory["services"] if _uuid(service["type"]) == kind]

        def characteristic(service, kind):
            found = [char for char in service["characteristics"] if _uuid(char["type"]) == kind]
            if len(found) != 1:
                raise HomeKitError("A required HomeKit characteristic is missing or ambiguous")
            return found[0]

        def unique(kind):
            found = services(kind)
            if len(found) != 1:
                raise HomeKitError("A required HomeKit service is missing or ambiguous")
            return found[0]

        information = unique("3E")
        if characteristic(information, "30").get("value") != serial:
            raise HomeKitError("HomeKit serial identity differs from the documented main")
        tv, speaker = unique("D8"), unique("113")
        self.chars = {"active": (aid, characteristic(tv, "B0")["iid"]),
                      "current_input": (aid, characteristic(tv, "E7")["iid"]),
                      "key": (aid, characteristic(tv, "E1")["iid"]),
                      "speaker_active": (aid, characteristic(speaker, "B0")["iid"]),
                      "mute": (aid, characteristic(speaker, "11A")["iid"]),
                      "volume": (aid, characteristic(speaker, "EA")["iid"])}
        self.inputs = [{name: (aid, characteristic(service, kind)["iid"])
                        for name, kind in (("index", "E6"), ("name", "E3"),
                                           ("configured", "D6"), ("visibility", "135"))}
                       for service in services("D9")]
        if not self.inputs:
            raise HomeKitError("HomeKit advertises no input services")


class _Session:
    def __init__(self, storage):
        self.storage = Path(storage)
        self.stack = AsyncExitStack()
        self.pairing = self.layout = None

    async def open(self, host, serial, pin, timeout):
        from zeroconf.asyncio import AsyncServiceBrowser, AsyncZeroconf
        from aiohomekit.characteristic_cache import CharacteristicCacheMemory
        from aiohomekit.controller.ip.controller import IpController
        from aiohomekit.zeroconf import ZeroconfServiceListener

        loop = asyncio.get_running_loop()
        addresses = {str(ipaddress.ip_address(row[4][0].split("%", 1)[0]))
                     for row in await loop.getaddrinfo(host, None, type=socket.SOCK_STREAM)}
        zeroconf = await self.stack.enter_async_context(AsyncZeroconf())
        browser = AsyncServiceBrowser(zeroconf.zeroconf, ["_hap._tcp.local."],
                                      listener=ZeroconfServiceListener())
        self.stack.push_async_callback(browser.async_cancel)
        controller = await self.stack.enter_async_context(IpController(CharacteristicCacheMemory(), zeroconf))
        deadline = loop.time() + timeout
        while True:
            matches = [discovery async for discovery in controller.async_discover()
                       if addresses.intersection(discovery.description.addresses)]
            if len(matches) > 1:
                raise HomeKitError("More than one HomeKit accessory matches the main address")
            if matches:
                break
            if loop.time() >= deadline:
                raise TimeoutError("The main did not advertise its HomeKit service")
            await asyncio.sleep(.1)
        finish = await matches[0].async_start_pairing("amity-functional")
        with pair_setup_srp():
            self.pairing = await finish(pin)
        # Persist ownership before the first encrypted read so a failed read
        # cannot lose the controller material needed for diagnosis/recovery.
        (self.storage / "pairing.json").write_text(json.dumps(self.pairing.pairing_data))
        accessories = await self.pairing.list_accessories_and_characteristics()
        self.layout = _Layout(accessories, serial)
        (self.storage / "verified").touch()
        return {"verified": True}

    async def _read(self, locations):
        # IDF retains the request line in its 256-byte URI scratch buffer
        # until the first header field is parsed. aiohomekit sends Host first,
        # so budget that prefix too. Plan every GET before sending any of them.
        batches, batch = [], []
        for location in dict.fromkeys(locations):
            candidate = [*batch, location]
            uri = "/characteristics?id=" + ",".join(f"{aid}.{iid}" for aid, iid in candidate)
            if len(f"GET {uri} HTTP/1.1\r\nHost: ") > 256:
                if not batch:
                    raise HomeKitError("A HomeKit characteristic identifier exceeds the URI bound")
                batches.append(batch)
                batch = [location]
            else:
                batch = candidate
        if batch:
            batches.append(batch)
        values = {}
        for batch in batches:
            values.update(await self.pairing.get_characteristics(batch))
        if any(location not in values or values[location].get("status", 0) != 0
               or "value" not in values[location] for location in locations):
            raise HomeKitError("HomeKit could not read the requested observed state")
        return {location: values[location]["value"] for location in locations}

    async def operation(self, operation, arguments):
        if self.layout is None:
            raise HomeKitError("HomeKit has no verified pairing")
        chars = self.layout.chars
        if operation == "read_inputs":
            locations = [chars["current_input"], *(location for row in self.layout.inputs for location in row.values())]
            values = await self._read(locations)
            inputs = [{"index": values[row["index"]], "name": values[row["name"]]}
                      for row in self.layout.inputs
                      if values[row["configured"]] == 1 and values[row["visibility"]] == 0]
            return {"inputs": inputs, "current_input": values[chars["current_input"]]}
        if operation == "read_state":
            values = await self._read([chars[name] for name in ("active", "current_input", "speaker_active")])
            return {"active": values[chars["active"]] == 1, "current_input": values[chars["current_input"]],
                    "speaker_active": values[chars["speaker_active"]] == 1}
        names = {"select_input": "current_input", "send_key": "key", "power": "active",
                 "mute": "mute", "volume": "volume"}
        location = chars[names[operation]]
        result = await self.pairing.put_characteristics([(*location, arguments["value"])])
        if not isinstance(result, dict) or any(key != location for key in result):
            raise HomeKitError("HomeKit returned a result for an unrelated characteristic")
        if location in result and (not isinstance(result[location], dict) or "status" not in result[location]):
            raise HomeKitError("HomeKit omitted a characteristic result status")
        status = result.get(location, {}).get("status", 0)
        if type(status) is not int:
            raise HomeKitError("HomeKit returned an invalid characteristic status")
        return status

    async def close(self):
        pairing, self.pairing = self.pairing, None
        try:
            if pairing is not None:
                try:
                    if await pairing.remove_pairing(pairing.pairing_data["iOSPairingId"]) is not True:
                        raise HomeKitError("HomeKit did not confirm removal of its test controller")
                    (self.storage / "pairing.json").unlink(missing_ok=True)
                finally:
                    await pairing.shutdown()
        finally:
            await self.stack.aclose()


class _Worker:
    """One private, persistent aiohomekit process with bounded request/reply I/O."""

    def __init__(self, *, timeout, storage=None, python=sys.executable):
        self.timeout, self.python = timeout, python
        self.storage = Path(storage or "~/.local/state/amity-harness/homekit").expanduser()
        self.process = self.log = self.directory = None
        self.sequence = 0
        self.buffer = bytearray()
        self.lock = threading.Lock()

    def start(self, device_id):
        self.storage.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.storage.stat().st_mode & 0o077:
            raise HomeKitError("HomeKit controller storage must be private")
        self.directory = Path(tempfile.mkdtemp(prefix=device_id + "-", dir=self.storage))
        self.log = (self.directory / "controller.log").open("x")
        (self.directory / "controller.log").chmod(0o600)
        command = [self.python, "-u", "-m", "amity_harness.homekit", "--worker",
                   "--storage", str(self.directory), "--timeout", str(self.timeout)]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, bufsize=0, start_new_session=True,
                                        env={**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])})
        os.set_blocking(self.process.stdin.fileno(), False)
        os.set_blocking(self.process.stdout.fileno(), False)

    def rpc(self, operation, arguments=None):
        deadline = time.monotonic() + self.timeout

        def remaining():
            value = deadline - time.monotonic()
            if value <= 0:
                raise TimeoutError("HomeKit worker deadline expired")
            return value

        if not self.lock.acquire(timeout=remaining()):
            raise TimeoutError("HomeKit worker is busy")
        try:
            if self.process is None:
                raise HomeKitError("HomeKit worker is not open")
            self.sequence += 1
            data = memoryview((json.dumps({"id": self.sequence, "operation": operation,
                                          "arguments": arguments or {}}) + "\n").encode())
            if len(data) > _MAX_MESSAGE:
                raise HomeKitError("HomeKit worker request is too large")
            while data:
                _, ready, _ = select.select([], [self.process.stdin], [], remaining())
                if ready:
                    data = data[os.write(self.process.stdin.fileno(), data):]
            while b"\n" not in self.buffer:
                ready, _, _ = select.select([self.process.stdout], [], [], remaining())
                if ready:
                    chunk = os.read(self.process.stdout.fileno(), _MAX_MESSAGE)
                    if not chunk:
                        raise HomeKitError("HomeKit worker ended unexpectedly")
                    self.buffer.extend(chunk)
                    if len(self.buffer) > _MAX_MESSAGE:
                        raise HomeKitError("HomeKit worker reply is too large")
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            reply = json.loads(line)
            if (not isinstance(reply, dict) or type(reply.get("id")) is not int
                    or reply["id"] != self.sequence or reply.get("ok") is not True or "result" not in reply):
                raise HomeKitError("HomeKit worker did not confirm its requested operation")
            return reply["result"]
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception:
            raise HomeKitError("HomeKit operation failed; inspect its private Pi controller log") from None
        finally:
            self.lock.release()

    def stop(self):
        process, self.process = self.process, None
        try:
            if process is not None:
                for stream in (process.stdin, process.stdout):
                    stream.close()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    _stop_process_group(process)
        finally:
            if self.log is not None:
                self.log.close()
                self.log = None


class HomeKitAccessory:
    """Pair only an unpaired selected main and remove only the fixture pairing."""

    def __init__(self, dut, *, timeout=None, worker_factory=None):
        if dut.name != dut._runtime.main_dut:
            raise ValueError("HomeKit accessory control requires the selected main DUT")
        self.dut = dut
        self.timeout = dut._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "HomeKit operation timeout")
        self.worker = (worker_factory or _Worker)(timeout=self.timeout)
        self._window = self._paired = self._closed = False

    def _status(self):
        state = self.dut.request("/api/homekit")
        flags = ("available", "started", "pairing", "busy", "paired")
        if (not isinstance(state, dict) or any(type(state.get(name)) is not bool for name in flags)
                or type(state.get("controllers")) is not int or state["controllers"] < 0):
            raise HomeKitError("The HomeKit UI returned no valid pairing state")
        if "error" in state:
            error = state["error"]
            if not isinstance(error, str) or re.fullmatch(r"ESP_[A-Z0-9_]{1,80}", error) is None:
                raise HomeKitError("The HomeKit UI returned no valid pairing state")
            event("homekit.status.failed", level=logging.ERROR, dut=self.dut.name, error=error,
                  **{name: state[name] for name in (*flags, "controllers")})
            raise HomeKitError("The HomeKit UI reported a pairing error")
        return state

    def _wait(self, predicate):
        deadline = time.monotonic() + self.timeout
        while True:
            state = self._status()
            if predicate(state):
                return state
            if time.monotonic() >= deadline:
                raise TimeoutError("HomeKit pairing state did not settle")
            time.sleep(.1)

    def __enter__(self):
        state = self._status()
        if state["paired"] or state["controllers"] or state["pairing"] or state["busy"]:
            raise HomeKitError("Existing HomeKit pairing or setup must be preserved")
        if not state["available"]:
            raise HomeKitError("HomeKit is unavailable before network enrollment")
        try:
            self._window = True
            if self.dut.request("/api/homekit/open", {}).get("ok") is not True:
                raise HomeKitError("The HomeKit UI did not open pairing")
            state = self._wait(lambda current: not current["busy"] and current["pairing"])
            pin = state.get("manual_code")
            if not isinstance(pin, str) or re.fullmatch(r"\d{3}-\d{2}-\d{3}", pin) is None:
                raise HomeKitError("The HomeKit UI supplied no pairing code")
            self.worker.start(self.dut.identity.device_id)
            result = self.worker.rpc("open", {"host": urlsplit(self.dut.url).hostname,
                                     "serial": self.dut.identity.mac.replace(":", "").upper(), "pin": pin})
            if result != {"verified": True}:
                raise HomeKitError("HomeKit did not verify the selected main")
            self._paired = True
            self._wait(lambda current: not current["busy"] and current["paired"] and current["controllers"] == 1)
            event("homekit.paired", dut=self.dut.name, verified=True)
            return self
        except BaseException:
            try:
                self.close()
            except BaseException as cleanup:
                event("homekit.cleanup.failed", dut=self.dut.name, error_type=type(cleanup).__name__)
            raise

    def _operation(self, operation, value=None):
        if self._closed or not self._paired:
            raise HomeKitError("HomeKit has no active fixture pairing")
        result = self.worker.rpc(operation, {} if value is None else {"value": value})
        if operation == "read_inputs":
            if (not isinstance(result, dict) or set(result) != {"inputs", "current_input"}
                    or type(result["current_input"]) is not int or not 0 <= result["current_input"] <= 16
                    or not isinstance(result["inputs"], list)
                    or any(not isinstance(row, dict) or set(row) != {"index", "name"}
                           or type(row["index"]) is not int or not 1 <= row["index"] <= 16
                           or not isinstance(row["name"], str) for row in result["inputs"])):
                raise HomeKitError("HomeKit returned invalid input characteristics")
        elif operation == "read_state":
            if (not isinstance(result, dict) or set(result) != {"active", "current_input", "speaker_active"}
                    or type(result["active"]) is not bool or type(result["speaker_active"]) is not bool
                    or type(result["current_input"]) is not int or not 0 <= result["current_input"] <= 16):
                raise HomeKitError("HomeKit returned invalid observed activity characteristics")
        elif type(result) is not int:
            raise HomeKitError("HomeKit returned no characteristic status")
        event("homekit.command", dut=self.dut.name, operation=operation,
              **({"status": result} if type(result) is int else {}))
        return result

    def read_inputs(self):
        return self._operation("read_inputs")

    def read_state(self):
        return self._operation("read_state")

    def select_input(self, index):
        return self._operation("select_input", _integer(index, 0xffffffff, "Input index"))

    def send_key(self, key):
        return self._operation("send_key", _integer(key, 255, "Remote key"))

    def set_power(self, on):
        if type(on) is not bool:
            raise ValueError("Power must be a boolean")
        return self._operation("power", int(on))

    def set_mute(self, muted):
        if type(muted) is not bool:
            raise ValueError("Mute must be a boolean")
        return self._operation("mute", muted)

    def step_volume(self, direction):
        if type(direction) is not int or direction not in (-1, 1):
            raise ValueError("Volume must step up or down once")
        return self._operation("volume", 0 if direction == 1 else 1)

    def close(self):
        if self._closed:
            return
        errors = []
        try:
            if self.worker.process is not None:
                try:
                    if self.worker.rpc("close") != {"closed": True}:
                        raise HomeKitError("HomeKit did not confirm pairing cleanup")
                except BaseException as error:
                    errors.append(error)
        finally:
            try:
                self.worker.stop()
            except BaseException as error:
                errors.append(error)
        if self._window:
            try:
                self.dut.request("/api/homekit/close", {})
                self._wait(lambda state: not state["busy"] and not state["pairing"])
                if not errors:
                    self._wait(lambda state: not state["busy"] and not state["paired"]
                               and state["controllers"] == 0)
            except BaseException as error:
                errors.append(error)
        self._closed = True
        if errors:
            raise HomeKitError("HomeKit fixture cleanup failed; inspect its private Pi controller log") from errors[0]
        if self._window:
            event("homekit.unpaired", dut=self.dut.name, removed=True)

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except BaseException:
            if exc is None:
                raise
            event("homekit.cleanup.failed", dut=self.dut.name, original_error=type(exc).__name__)
        return False


async def _worker_main(storage, timeout, output):
    session = _Session(storage)
    try:
        while True:
            line = await asyncio.to_thread(sys.stdin.buffer.readline, _MAX_MESSAGE + 1)
            if not line:
                break
            request = json.loads(line)
            identifier = request.get("id")
            operation, arguments = request.get("operation"), request.get("arguments")
            try:
                if (len(line) > _MAX_MESSAGE or type(identifier) is not int or identifier < 1
                        or not isinstance(arguments, dict)):
                    raise ValueError("Invalid HomeKit worker request")
                if operation == "open":
                    result = await asyncio.wait_for(session.open(arguments["host"], arguments["serial"],
                                                                 arguments["pin"], timeout), timeout)
                elif operation == "close":
                    await asyncio.wait_for(session.close(), timeout)
                    result = {"closed": True}
                elif operation in ("read_inputs", "read_state", "select_input", "send_key", "power", "mute", "volume"):
                    result = await asyncio.wait_for(session.operation(operation, arguments), timeout)
                else:
                    raise ValueError("Unknown HomeKit worker operation")
                reply = {"id": identifier, "ok": True, "result": result}
            except Exception as error:
                traceback.print_exc()
                reply = {"id": identifier, "ok": False, "error_type": type(error).__name__}
            output.write(json.dumps(reply) + "\n")
            output.flush()
            if operation in ("close", "open") and (operation == "close" or not reply["ok"]):
                break
    finally:
        await asyncio.wait_for(session.close(), timeout)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", action="store_true", required=True)
    parser.add_argument("--storage", required=True)
    parser.add_argument("--timeout", type=float, required=True)
    args = parser.parse_args(argv)
    _positive_seconds(args.timeout, "HomeKit worker timeout")
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO)
    output = sys.stdout
    with redirect_stdout(sys.stderr):
        asyncio.run(_worker_main(args.storage, args.timeout, output))


if __name__ == "__main__":
    main()
