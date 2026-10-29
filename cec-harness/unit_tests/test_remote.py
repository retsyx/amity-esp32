"""CEC-only remote protocol checks using local pipes, never SSH or hardware."""

import io
import json
from pathlib import Path
import subprocess
import sys
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from amity_harness.cec import CecError, Frame, TxResult
from amity_harness.remote import SSHCecBackend, _component_descriptor, _serve
from amity_harness.topology import CecAddress


class WorkerBackend:
    def __init__(self, gpio):
        self.gpio = gpio
        self.opened = False
        self.close_count = 0
        self.configured = []

    def open(self):
        self.opened = True

    def configure(self, components):
        self.configured = components

    def transmit(self, frame):
        return TxResult(frame, 1, 12345)

    def receive(self, timeout):
        return Frame.from_bytes(bytes.fromhex("0546"), timestamp_ns=12346)

    def close(self):
        self.close_count += 1


def request(request_id, operation, **arguments):
    return json.dumps({"id": request_id, "operation": operation, "arguments": arguments}) + "\n"


_PIPE_WORKER = r'''
import sys
import time
from amity_harness.cec import Frame, TxResult
from amity_harness.remote import _serve
def clock():
    return time.monotonic_ns() + 50_000_000_000
class Backend:
    def __init__(self, gpio):
        self.gpio = gpio
    def open(self):
        pass
    def configure(self, components):
        if components and components[0].address.logical == 14:
            raise RuntimeError("synthetic configuration failure")
    def transmit(self, frame):
        return TxResult(frame, 1, clock())
    def receive(self, timeout):
        return Frame(0, 5, 0x46, timestamp_ns=clock())
    def close(self):
        pass
_serve(24, sys.stdin, sys.stdout, backend_factory=Backend, clock=clock)
'''


class RemoteProtocolTests(unittest.TestCase):
    def test_worker_round_trip_and_only_cec_configuration_fields(self):
        backend = WorkerBackend(24)
        descriptor = {"logical": 5, "physical": 4096, "device_type": 5,
                      "logical_address_type": 4, "all_device_types": 8}
        commands = (request(1, "open") + request(2, "clock")
                    + request(3, "configure", components=[descriptor])
                    + request(4, "transmit", frame="508f")
                    + request(5, "receive", timeout=0.05) + request(6, "close"))
        output = io.StringIO()
        _serve(24, io.StringIO(commands), output, backend_factory=lambda gpio: backend, clock=lambda: 1000)
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertTrue(all(response["ok"] for response in responses))
        self.assertEqual(responses[1]["result"], {"remote_monotonic_ns": 1000})
        self.assertEqual(responses[3]["result"], {"status": 1, "timestamp_ns": 12345})
        self.assertEqual(responses[4]["result"], {"frame": "0546", "timestamp_ns": 12346})
        self.assertEqual(backend.configured[0].address, CecAddress(5, 4096))
        self.assertGreaterEqual(backend.close_count, 1)

    def test_worker_rejects_credentials_and_always_cleans_up_on_failure_or_eof(self):
        for extra in (request(2, "http", password="must never be accepted"),
                      request(2, "configure", components=[], password="must never be accepted"),
                      ""):
            with self.subTest(extra=extra):
                backend = WorkerBackend(24)
                output = io.StringIO()
                _serve(24, io.StringIO(request(1, "open") + extra), output,
                       backend_factory=lambda gpio: backend)
                replies = [json.loads(line) for line in output.getvalue().splitlines()]
                if extra:
                    self.assertFalse(replies[-1]["ok"])
                self.assertGreaterEqual(backend.close_count, 1)

    def test_worker_rejects_invalid_frames_and_request_sizes(self):
        for invalid in (request(2, "transmit", frame=""),
                        request(2, "receive", timeout=100),
                        "x" * 65537 + "\n"):
            backend = WorkerBackend(24)
            output = io.StringIO()
            _serve(24, io.StringIO(request(1, "open") + invalid), output,
                   backend_factory=lambda gpio: backend)
            self.assertFalse(json.loads(output.getvalue().splitlines()[-1])["ok"])
            self.assertGreaterEqual(backend.close_count, 1)

    def test_ssh_command_quotes_root_and_contains_no_dut_authentication(self):
        backend = SSHCecBackend(24, host="pi@example.local", root="/tmp/a 'quoted' harness", port=2222)
        command = backend._command()
        self.assertEqual(command[-2], "pi@example.local")
        self.assertIn("StrictHostKeyChecking=yes", command)
        self.assertEqual(command[command.index("-p") + 1], "2222")
        import shlex
        self.assertEqual(shlex.split(command[-1]), ["env", "PYTHONPATH=/tmp/a 'quoted' harness",
                                                   "python3", "-u", "-m", "amity_harness.remote", "--gpio", "24"])
        component = SimpleNamespace(address=CecAddress(5, "1.0.0.0"), device_type=5,
                                    logical_address_type=4, all_device_types=8,
                                    dut=SimpleNamespace(password="private", token="private"))
        self.assertEqual(set(_component_descriptor(component)),
                         {"logical", "physical", "device_type", "logical_address_type", "all_device_types"})
        for host in ("-oProxyCommand=bad", "pi@host extra", ""):
            with self.assertRaises(ValueError):
                SSHCecBackend(24, host=host)

    def _local_transport(self, source=_PIPE_WORKER, timeout=2):
        backend = SSHCecBackend(24, rpc_timeout=timeout)
        real_popen = subprocess.Popen
        framework = str(Path(__file__).resolve().parents[1])

        def launch(_command, **kwargs):
            # Exercise the real pipe framing/deadlines with a local subprocess;
            # ignore the constructed SSH argv so no remote process is invoked.
            return real_popen([sys.executable, "-u", "-c",
                               f"import sys; sys.path.insert(0, {framework!r})\n" + source], **kwargs)

        return backend, patch("amity_harness.remote.subprocess.Popen", side_effect=launch)

    def test_transport_maps_remote_clock_and_exchanges_real_local_pipe_messages(self):
        backend, launcher = self._local_transport()
        with launcher:
            backend.open()
            process = backend._process
            try:
                self.assertLess(abs(backend.clock_offset_ns + 50_000_000_000), 100_000_000)
                self.assertGreaterEqual(backend.clock_uncertainty_ns, 0)
                component = SimpleNamespace(address=CecAddress(5, 4096), device_type=5,
                                            logical_address_type=4, all_device_types=8)
                backend.configure([component])
                before = time.monotonic_ns()
                frame = backend.receive()
                after = time.monotonic_ns()
                self.assertEqual(frame.raw, bytes.fromhex("0546"))
                self.assertLessEqual(frame.timestamp_ns, after)
                self.assertGreaterEqual(frame.timestamp_ns, before - 100_000_000)
                sent = backend.transmit(Frame(5, 0, 0x90, b"\x01"))
                self.assertTrue(sent.ok)
                self.assertLessEqual(sent.timestamp_ns, time.monotonic_ns())
            finally:
                backend.close()
            self.assertIsNotNone(process.poll())
            self.assertIsNone(backend._process)

    def test_transport_remote_error_and_unexpected_eof_terminate_processes(self):
        backend, launcher = self._local_transport()
        with launcher:
            backend.open()
            process = backend._process
            bad = SimpleNamespace(address=CecAddress(14, 4096), device_type=7,
                                  logical_address_type=5, all_device_types=0)
            with self.assertRaisesRegex(CecError, "synthetic configuration failure"):
                backend.configure([bad])
            self.assertIsNone(backend._process)
            self.assertIsNotNone(process.poll())
        backend, launcher = self._local_transport("pass\n")
        with launcher, self.assertRaisesRegex(CecError, "closed"):
            backend.open()
        self.assertIsNone(backend._process)

    def test_transport_timeout_is_bounded_and_cleans_up(self):
        backend, launcher = self._local_transport("import time; time.sleep(30)\n", timeout=0.05)
        start = time.monotonic()
        with launcher, self.assertRaisesRegex(CecError, "Timed out"):
            backend.open()
        self.assertLess(time.monotonic() - start, 6)
        self.assertIsNone(backend._process)


if __name__ == "__main__":
    unittest.main()
