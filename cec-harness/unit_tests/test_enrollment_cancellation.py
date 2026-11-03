"""Matter workers stop native descendants before releasing the BLE adapter."""

import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from amity_harness.enrollment import _run_process_group, _worker, _worker_signals, _WorkerInterrupted


class CancellationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="amity-enrollment-cancel-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def running(self, pid):
        try:
            os.kill(pid, 0)
            status = Path(f"/proc/{pid}/stat")
            if status.exists():
                return status.read_text().rsplit(")", 1)[1].split()[0] != "Z"
        except ProcessLookupError:
            return False
        return True

    def assert_stopped(self, pid):
        deadline = time.monotonic() + 2
        while self.running(pid) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(self.running(pid))

    def stop_pid(self, pid):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def wait_file(self, path, timeout=5):
        deadline = time.monotonic() + timeout
        while not path.exists():
            if time.monotonic() >= deadline:
                self.fail("Dummy worker did not reach its marker")
            time.sleep(0.02)
        return path.read_text()

    def native_command(self, *, ignore_term=True):
        marker = self.root / "native.pid"
        script = ("import os,signal,sys,time; from pathlib import Path; "
                  + ("signal.signal(signal.SIGTERM,signal.SIG_IGN); " if ignore_term else "")
                  + "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(30)")
        return [sys.executable, "-c", script, str(marker)], marker

    def descendant_command(self):
        command, marker = self.native_command()
        # The launcher exits on TERM, while its child deliberately ignores it.
        # Cleanup must still KILL the surviving member of the same group.
        script = "import json,subprocess,sys,time; subprocess.Popen(json.loads(sys.argv[1])); time.sleep(30)"
        return [sys.executable, "-c", script, json.dumps(command)], marker

    def test_timeout_stops_descendant_even_after_launcher_exits_on_term(self):
        command, marker = self.descendant_command()
        with self.assertRaises(subprocess.TimeoutExpired):
            _run_process_group(command, timeout=0.5, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pid = int(self.wait_file(marker))
        self.addCleanup(self.stop_pid, pid)
        self.assert_stopped(pid)

    def test_interruption_stops_native_group_before_adapter_lock_is_released(self):
        command, marker = self.descendant_command()
        sdk, paa = self.root / "sdk", self.root / "paa"
        sdk.mkdir()
        paa.mkdir()
        adapter = 252
        lock = Path(tempfile.gettempdir()) / f"amity-ble-hci{adapter}.lock"
        args = SimpleNamespace(storage=str(self.root / "matter"), sdk_path=str(sdk), paa_path=str(paa),
                               timeout=20, ble_controller=adapter)
        request = {"protocol_version": 1, "request_id": "1" * 32, "operation": "enroll",
                   "device_id": "a" * 32,
                   "credentials": {"wifi_ssid": "unused", "wifi_password": "unused password",
                                   "matter_pin_code": 20202021, "matter_discriminator": 3840}}

        def native(unused_command, **options):
            timer = threading.Timer(0.5, os.kill, args=(os.getpid(), signal.SIGTERM))
            timer.start()
            try:
                return _run_process_group(command, **options)
            except _WorkerInterrupted:
                pid = int(self.wait_file(marker))
                self.addCleanup(self.stop_pid, pid)
                self.assert_stopped(pid)
                with lock.open("r+") as other:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
                raise
            finally:
                timer.cancel()
                timer.join()

        # Do not arm parent-death signaling on the unittest process itself.
        with patch("amity_harness.enrollment.sys.platform", "darwin"), _worker_signals():
            with self.assertRaises(_WorkerInterrupted):
                _worker(request, args, runner=native)
        with lock.open("r+") as other:
            fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_worker_restores_signal_handlers_after_interruption(self):
        for signum in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM):
            before = signal.getsignal(signum)
            with self.subTest(signum=signum), patch("amity_harness.enrollment.sys.platform", "darwin"):
                with self.assertRaises(_WorkerInterrupted), _worker_signals():
                    os.kill(os.getpid(), signum)
                self.assertIs(signal.getsignal(signum), before)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux Pi parent-death signaling")
    def test_parent_exit_stops_remote_worker_and_native_group(self):
        command, native_marker = self.native_command()
        stopped = self.root / "worker-stopped"
        worker_marker = self.root / "worker.pid"
        worker = """import json,subprocess,sys
from pathlib import Path
from amity_harness.enrollment import _run_process_group,_worker_signals,_WorkerInterrupted
try:
    with _worker_signals():
        _run_process_group(json.loads(sys.argv[1]),timeout=25,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
except _WorkerInterrupted:
    Path(sys.argv[2]).write_text('stopped')
"""
        parent = ("import subprocess,sys,time; from pathlib import Path; "
                  "p=subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2],sys.argv[3]]); "
                  "Path(sys.argv[4]).write_text(str(p.pid)); time.sleep(30)")
        launcher = subprocess.Popen([sys.executable, "-c", parent, worker, json.dumps(command),
                                     str(stopped), str(worker_marker)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_pid, launcher.pid)
        worker_pid = int(self.wait_file(worker_marker))
        self.addCleanup(self.stop_pid, worker_pid)
        native_pid = int(self.wait_file(native_marker))
        self.addCleanup(self.stop_pid, native_pid)
        launcher.terminate()
        launcher.wait(timeout=5)
        self.assertEqual(self.wait_file(stopped, timeout=8), "stopped")
        self.assert_stopped(native_pid)


if __name__ == "__main__":
    unittest.main()
