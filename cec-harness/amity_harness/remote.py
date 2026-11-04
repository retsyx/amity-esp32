"""CEC-only SSH transport; DUT HTTPS authentication stays on the calling host.

The Pi worker receives CEC adapter configuration and raw CEC frames only. It
never receives DUT passwords, login tokens, HTTP requests or credentials files.
Run it through SSHCecBackend rather than launching a persistent simulator.
"""

import argparse
import json
import math
import os
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

from .cec import CecError, Frame, LinuxCecBackend, TxResult
from .topology import CecAddress


_MAX_LINE = 65536
_RPC_VERSION = 2


def _integer(value, name, minimum, maximum):
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"Invalid {name}")
    return value


def _keys(value, names):
    if not isinstance(value, dict) or set(value) != set(names):
        raise ValueError("Unexpected CEC RPC fields")


def _cec_version(value):
    if value is not None and (isinstance(value, bool) or not isinstance(value, int) or value not in (5, 6)):
        raise ValueError("Invalid CEC version metadata")
    return value


def _component_descriptor(component):
    return {"logical": component.address.logical, "physical": component.address.physical,
            "device_type": component.device_type,
            "logical_address_type": component.logical_address_type,
            "all_device_types": component.all_device_types}


def _decode_component(value):
    _keys(value, ("logical", "physical", "device_type", "logical_address_type", "all_device_types"))
    address = CecAddress(_integer(value["logical"], "logical address", 0, 14),
                         _integer(value["physical"], "physical address", 0, 65534))
    return SimpleNamespace(address=address,
                           device_type=_integer(value["device_type"], "device type", 0, 7),
                           logical_address_type=_integer(value["logical_address_type"], "logical address type", 0, 5),
                           all_device_types=_integer(value["all_device_types"], "device-type mask", 0, 255))


class SSHCecBackend:
    """Run LinuxCecBackend on a Pi while all DUT HTTP calls remain local.

    The factory signature matches CecSession's backend_factory argument. Each
    wire gets one SSH process and one exclusive Linux CEC descriptor. The RPC
    is serialized, line bounded and deadline bounded; process EOF, invalid JSON
    and remote errors terminate this transport rather than silently continuing.

    ``clock_offset_ns`` is the handshake midpoint estimate (local minus remote);
    ``clock_uncertainty_ns`` bounds that estimate by half the handshake RTT.
    Frame timestamps use the lower bound, conservatively excluding ambiguous
    boundary traffic from assertions that demand fresh post-scan messages.
    """

    def __init__(self, gpio, *, host="pi@test2.local",
                 root="/home/pi/amity/esp32/cec-harness", port=None,
                 rpc_timeout=20.0):
        self.gpio = _integer(gpio, "GPIO", 0, 255)
        if not isinstance(host, str) or not host or host.startswith("-") or any(c.isspace() for c in host):
            raise ValueError("Invalid SSH host")
        if not isinstance(root, str) or not root.startswith("/") or "\x00" in root:
            raise ValueError("The remote harness root must be an absolute path")
        if port is not None:
            _integer(port, "SSH port", 1, 65535)
        if not math.isfinite(rpc_timeout) or rpc_timeout <= 0:
            raise ValueError("rpc_timeout must be a positive finite number")
        self.host, self.root, self.port = host, root, port
        self.rpc_timeout = rpc_timeout
        self.clock_offset_ns = None
        self.clock_uncertainty_ns = None
        self.cec_version = None
        self._monitor_opened = False
        self._last_pin_diagnostic = None
        self._process = None
        self._stderr = None
        self._buffer = bytearray()
        self._sequence = 0
        self._lock = threading.RLock()

    def _command(self):
        remote = shlex.join(["env", f"PYTHONPATH={self.root}", "python3", "-u", "-m",
                             "amity_harness.remote", "--gpio", str(self.gpio)])
        return ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                "-o", "StrictHostKeyChecking=yes",
                *(["-p", str(self.port)] if self.port else []), self.host, remote]

    def open(self):
        with self._lock:
            if self._process is not None:
                raise CecError("Remote CEC adapter is already open")
            self.clock_offset_ns = self.clock_uncertainty_ns = None
            self.cec_version = None
            self._buffer.clear()
            self._stderr = tempfile.TemporaryFile()
            self._last_pin_diagnostic = None
            try:
                self._process = subprocess.Popen(self._command(), stdin=subprocess.PIPE,
                                                 stdout=subprocess.PIPE, stderr=self._stderr,
                                                 bufsize=0)
                os.set_blocking(self._process.stdin.fileno(), False)
                os.set_blocking(self._process.stdout.fileno(), False)
                result = self._rpc("open", {"protocol_version": _RPC_VERSION})
                try:
                    _keys(result, ("protocol_version",))
                    _integer(result["protocol_version"], "CEC RPC protocol version", _RPC_VERSION, _RPC_VERSION)
                except (TypeError, ValueError) as exc:
                    self._bad_result("open", exc)
                samples = []
                # Exclude SSH connection/module import/adapter acquisition from
                # clock uncertainty; sample the live RPC after opening succeeds.
                for _ in range(3):
                    before = time.monotonic_ns()
                    result = self._rpc("clock", {})
                    after = time.monotonic_ns()
                    _keys(result, ("remote_monotonic_ns",))
                    remote_ns = _integer(result["remote_monotonic_ns"], "remote clock", 0, 2**64 - 1)
                    samples.append((after - before, (before + after) // 2 - remote_ns))
                round_trip, self.clock_offset_ns = min(samples)
                self.clock_uncertainty_ns = (round_trip + 1) // 2
                return self
            except BaseException:
                self._shutdown()
                raise

    def _diagnostic(self):
        if self._stderr is None:
            return ""
        self._stderr.seek(0)
        return self._stderr.read(4096).decode("utf-8", "replace").strip()

    def _write(self, packet, deadline):
        fd = self._process.stdin.fileno()
        remaining = memoryview(packet)
        while remaining:
            timeout = deadline - time.monotonic()
            if timeout <= 0:
                raise TimeoutError("Timed out writing a CEC RPC request")
            _, writable, _ = select.select([], [fd], [], timeout)
            if not writable:
                raise TimeoutError("Timed out writing a CEC RPC request")
            try:
                count = os.write(fd, remaining)
            except BlockingIOError:
                continue
            if not count:
                raise EOFError("Pi CEC worker input closed")
            remaining = remaining[count:]

    def _read(self, deadline):
        fd = self._process.stdout.fileno()
        while b"\n" not in self._buffer:
            if len(self._buffer) > _MAX_LINE:
                raise ValueError("Pi CEC worker response exceeded the line limit")
            timeout = deadline - time.monotonic()
            if timeout <= 0:
                raise TimeoutError("Timed out waiting for the Pi CEC worker")
            readable, _, _ = select.select([fd], [], [], timeout)
            if not readable:
                raise TimeoutError("Timed out waiting for the Pi CEC worker")
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                raise EOFError("Pi CEC worker output closed")
            self._buffer.extend(chunk)
        line, _, rest = self._buffer.partition(b"\n")
        if len(line) > _MAX_LINE:
            raise ValueError("Pi CEC worker response exceeded the line limit")
        self._buffer = bytearray(rest)
        return json.loads(line)

    def _rpc(self, operation, arguments, *, timeout=None):
        with self._lock:
            if self._process is None:
                error = CecError("Remote CEC adapter is not open")
                if self._last_pin_diagnostic is not None:
                    error.pin_diagnostic = self.last_pin_diagnostic
                raise error
            self._sequence += 1
            request_id = self._sequence
            packet = (json.dumps({"id": request_id, "operation": operation,
                                  "arguments": arguments}, separators=(",", ":")) + "\n").encode()
            if len(packet) > _MAX_LINE:
                raise CecError("CEC RPC request exceeded the line limit")
            deadline = time.monotonic() + (self.rpc_timeout if timeout is None else timeout)
            try:
                self._write(packet, deadline)
                response = self._read(deadline)
                if not isinstance(response, dict) or response.get("id") != request_id:
                    raise ValueError("Pi CEC worker returned an unexpected request ID")
                if response.get("ok") is not True:
                    error = CecError(str(response.get("error", "Pi CEC worker failed")))
                    if "pin_diagnostic" in response:
                        from .cec_pin import validate_pin_diagnostic
                        self._last_pin_diagnostic = validate_pin_diagnostic(response["pin_diagnostic"])
                        error.pin_diagnostic = self.last_pin_diagnostic
                    raise error
                if "result" not in response:
                    raise ValueError("Pi CEC worker omitted its result")
                return response["result"]
            except BaseException as exc:
                diagnostic = self._diagnostic()
                self._shutdown()
                if isinstance(exc, (KeyboardInterrupt, SystemExit)):
                    raise
                error = CecError(f"CEC RPC {operation} failed: {exc}"
                                 + (f"; SSH: {diagnostic}" if diagnostic else ""))
                if self._last_pin_diagnostic is not None:
                    error.pin_diagnostic = self.last_pin_diagnostic
                raise error from exc

    @property
    def last_pin_diagnostic(self):
        if self._last_pin_diagnostic is None:
            return None
        from .cec_pin import validate_pin_diagnostic
        return validate_pin_diagnostic(self._last_pin_diagnostic)

    def _local_timestamp(self, remote_ns):
        remote_ns = _integer(remote_ns, "wire timestamp", 0, 2**64 - 1)
        if not remote_ns:
            return 0
        if self.clock_offset_ns is None:
            raise CecError("Remote CEC clock has not been synchronized")
        return max(1, remote_ns + self.clock_offset_ns - self.clock_uncertainty_ns)

    def configure(self, components):
        with self._lock:
            result = self._rpc("configure", {"components": [_component_descriptor(c) for c in components]})
            try:
                _keys(result, ("cec_version",))
                self.cec_version = _cec_version(result["cec_version"])
            except (TypeError, ValueError) as exc:
                self._bad_result("configure", exc)

    def _bad_result(self, operation, error):
        with self._lock:
            self._shutdown()
        raise CecError(f"Malformed Pi CEC {operation} result: {error}") from error

    def transmit(self, frame):
        result = self._rpc("transmit", {"frame": frame.raw.hex()})
        try:
            _keys(result, ("status", "timestamp_ns"))
            return TxResult(frame, _integer(result["status"], "TX status", 0, 255),
                            self._local_timestamp(result["timestamp_ns"]))
        except (TypeError, ValueError) as exc:
            self._bad_result("transmit", exc)

    def receive(self, timeout=0.05):
        if not math.isfinite(timeout) or not 0 <= timeout <= 1:
            raise ValueError("Remote CEC receive timeout must be between 0 and 1 second")
        # A short receive must remain interruptible during session shutdown;
        # don't spend the full configuration/TX deadline awaiting an idle wire.
        result = self._rpc("receive", {"timeout": timeout},
                           timeout=min(self.rpc_timeout, timeout + 1.0))
        if result is None:
            return None
        try:
            _keys(result, ("frame", "timestamp_ns"))
            return Frame.from_bytes(bytes.fromhex(result["frame"]),
                                    timestamp_ns=self._local_timestamp(result["timestamp_ns"]))
        except (TypeError, ValueError) as exc:
            self._bad_result("receive", exc)

    def open_monitor(self):
        if self._monitor_opened:
            raise CecError("The remote CEC monitor is already open")
        result = self._rpc("monitor_open", {})
        if result is not None:
            self._bad_result("monitor_open", ValueError("Unexpected monitor metadata"))
        self._monitor_opened = True

    def receive_monitor(self, timeout=0.05):
        if not self._monitor_opened:
            raise CecError("The remote CEC monitor is not open")
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 <= timeout <= 1:
            raise ValueError("Remote CEC monitor timeout must be between 0 and 1 second")
        result = self._rpc("monitor_receive", {"timeout": timeout}, timeout=min(self.rpc_timeout, timeout + 1.0))
        if result is None:
            return None
        try:
            from .cec_pin import PinFrame
            _keys(result, ("frame", "timestamp_ns", "eom", "ack", "synchronized_ns"))
            return PinFrame.observed(bytes.fromhex(result["frame"]),
                                     timestamp_ns=self._local_timestamp(result["timestamp_ns"]),
                                     eom=result["eom"], ack=result["ack"],
                                     synchronized_ns=self._local_timestamp(result["synchronized_ns"]))
        except (TypeError, ValueError) as exc:
            self._bad_result("monitor_receive", exc)

    def close_monitor(self):
        if self._monitor_opened:
            self._monitor_opened = False
            if self._process is not None and self._rpc("monitor_close", {}) is not None:
                self._bad_result("monitor_close", ValueError("Unexpected monitor metadata"))

    def _shutdown(self):
        self.cec_version = None
        self._monitor_opened = False
        process, self._process = self._process, None
        if process is not None:
            try:
                # Closing stdin gives the worker EOF and its adapter cleanup
                # finally block before escalating process termination.
                process.stdin.close()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
            finally:
                process.stdout.close()
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None

    def close(self):
        with self._lock:
            if self._process is None:
                self.cec_version = None
                return
            try:
                self._rpc("close", {})
            finally:
                self._shutdown()


def _serve(gpio, reader, writer, *, backend_factory=LinuxCecBackend,
           clock=time.monotonic_ns):
    """Serve the small CEC-only protocol; injectable streams/backend aid unit QA."""
    backend = backend_factory(gpio)
    opened = False
    try:
        while True:
            line = reader.readline(_MAX_LINE + 1)
            if not line:
                break
            request_id = None
            try:
                if len(line) > _MAX_LINE or not line.endswith("\n"):
                    raise ValueError("CEC RPC request exceeded the line limit or lacked a newline")
                request = json.loads(line)
                _keys(request, ("id", "operation", "arguments"))
                request_id = _integer(request["id"], "request ID", 1, 2**53 - 1)
                operation, arguments = request["operation"], request["arguments"]
                if operation == "open":
                    _keys(arguments, ("protocol_version",))
                    _integer(arguments["protocol_version"], "CEC RPC protocol version", _RPC_VERSION, _RPC_VERSION)
                    if opened:
                        raise ValueError("CEC adapter already opened")
                    backend.open()
                    opened = True
                    result = {"protocol_version": _RPC_VERSION}
                elif operation == "close":
                    _keys(arguments, ())
                    backend.close()
                    opened = False
                    result = None
                elif not opened:
                    raise ValueError("CEC adapter has not been opened")
                elif operation == "clock":
                    _keys(arguments, ())
                    result = {"remote_monotonic_ns": clock()}
                elif operation == "configure":
                    _keys(arguments, ("components",))
                    values = arguments["components"]
                    if not isinstance(values, list) or len(values) > 4:
                        raise ValueError("Invalid CEC component descriptors")
                    backend.configure([_decode_component(value) for value in values])
                    result = {"cec_version": _cec_version(getattr(backend, "cec_version", None))}
                elif operation == "transmit":
                    _keys(arguments, ("frame",))
                    frame = Frame.from_bytes(bytes.fromhex(arguments["frame"]))
                    transmitted = backend.transmit(frame)
                    result = {"status": transmitted.status, "timestamp_ns": transmitted.timestamp_ns}
                elif operation == "receive":
                    _keys(arguments, ("timeout",))
                    timeout = arguments["timeout"]
                    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 <= timeout <= 1:
                        raise ValueError("Invalid CEC receive timeout")
                    frame = backend.receive(timeout)
                    result = None if frame is None else {"frame": frame.raw.hex(), "timestamp_ns": frame.timestamp_ns}
                elif operation == "monitor_open":
                    _keys(arguments, ())
                    backend.open_monitor()
                    result = None
                elif operation == "monitor_close":
                    _keys(arguments, ())
                    backend.close_monitor()
                    result = None
                elif operation == "monitor_receive":
                    _keys(arguments, ("timeout",))
                    timeout = arguments["timeout"]
                    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 <= timeout <= 1:
                        raise ValueError("Invalid CEC monitor timeout")
                    frame = backend.receive_monitor(timeout)
                    result = None if frame is None else {"frame": frame.raw.hex(), "timestamp_ns": frame.timestamp_ns,
                                                         "eom": frame.eom, "ack": frame.ack,
                                                         "synchronized_ns": frame.synchronized_ns}
                else:
                    raise ValueError("Unsupported CEC RPC operation")
                response = {"id": request_id, "ok": True, "result": result}
            except Exception as exc:
                response = {"id": request_id, "ok": False, "error": str(exc)}
                from .cec_pin import pin_diagnostic
                diagnostic = pin_diagnostic(exc)
                if diagnostic is not None:
                    response["pin_diagnostic"] = diagnostic
                writer.write(json.dumps(response, separators=(",", ":")) + "\n")
                writer.flush()
                break
            writer.write(json.dumps(response, separators=(",", ":")) + "\n")
            writer.flush()
            if operation == "close":
                break
    finally:
        backend.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description="CEC-only stdio worker for SSHCecBackend")
    parser.add_argument("--gpio", type=int, required=True)
    args = parser.parse_args(argv)
    _integer(args.gpio, "GPIO", 0, 255)

    def terminate(signum, _frame):
        raise SystemExit(128 + signum)

    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, terminate)
    _serve(args.gpio, sys.stdin, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
