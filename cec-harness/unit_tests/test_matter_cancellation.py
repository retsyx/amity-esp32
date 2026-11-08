"""Run the real Matter IPC/supervisor against an unprivileged dummy native tree."""

import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.matter import MatterError, _Worker


DEVICE = "a" * 32
REAL_POPEN = subprocess.Popen

NATIVE = r'''
import json, os, signal, subprocess, sys, time
from pathlib import Path
root, response = map(Path, sys.argv[1:3])
signal.signal(signal.SIGTERM, signal.SIG_IGN)
(root / "native.pid").write_text(str(os.getpid()))
subprocess.Popen([sys.executable, "-c", "import os,signal,sys,time; from pathlib import Path; "
    "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
    "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(30)", str(root / "descendant.pid")])
while not (root / "descendant.pid").exists():
    time.sleep(.01)
for line in sys.stdin:
    request = json.loads(line)
    with (root / "commands.jsonl").open("a") as log:
        log.write(json.dumps(request) + "\n")
    operation = request["operation"]
    if operation == "send_key":
        # Model an uncertain mutation: it ran, but its result never arrives.
        continue
    result = {"opened": True} if operation == "open" else {"closed": True}
    reply = {key: request[key] for key in ("request_id", "device_id", "operation")}
    reply["result"] = result
    temporary = response.with_suffix(".tmp")
    temporary.write_text(json.dumps(reply))
    temporary.replace(response)
    if operation == "close":
        # Stay alive so real process-group cleanup must also stop the launcher.
        time.sleep(30)
'''

SUPERVISOR = r'''
import fcntl, os, signal, subprocess, sys
from pathlib import Path
from amity_harness import matter
root = Path(os.environ["AMITY_CANCELLATION_ROOT"])
real_popen, real_stop = subprocess.Popen, matter._stop_process_group

def native(command, **options):
    assert command[:6] == ["sudo", "-n", "unshare", "--mount", "--propagation", "private"]
    response = command[command.index("--response") + 1]
    with (root / "launches").open("a") as log:
        log.write("native\n")
    return real_popen([sys.executable, str(root / "native.py"), str(root), response], **options)

def locked():
    with (root / "fabric" / "accessory.lock").open("a") as other:
        try:
            fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
    raise AssertionError("The fabric lock escaped native cleanup")

def stop(process):
    if os.environ.get("AMITY_CANCELLATION_CLEANUP_SIGNAL"):
        # Cancel at cleanup entry, before the existing process-group helper
        # masks signals. The supervisor must still stop native descendants.
        os.kill(os.getpid(), signal.SIGTERM)
    locked()
    (root / "cleanup-started").touch()
    try:
        real_stop(process)
    finally:
        # A TERM pending during the helper's signal mask can be delivered as
        # that mask is restored, after it already stopped the native group.
        locked()
        (root / "cleanup-finished").touch()
    if os.environ.get("AMITY_CANCELLATION_FAILURE"):
        raise RuntimeError("injected cleanup failure after stopping native children")

matter.subprocess.Popen = native
matter._stop_process_group = stop
raise SystemExit(matter.main(sys.argv[1:]))
'''


class MatterCancellationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="amity-matter-cancellation-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.root.chmod(0o700)
        self.storage = self.root / "storage"
        owner = self.storage / DEVICE
        owner.mkdir(parents=True, mode=0o700)
        self.storage.chmod(0o700)
        self.fabric = owner / "generation-test"
        self.fabric.mkdir(mode=0o700)
        (self.fabric / "platform").mkdir(mode=0o700)
        (self.fabric / "controller.json").write_text("private saved fabric")
        (owner / "current").symlink_to(self.fabric.name)
        (self.root / "fabric").symlink_to(self.fabric)
        self.sdk, self.paa = self.root / "sdk", self.root / "paa"
        self.sdk.mkdir()
        self.paa.mkdir()
        (self.root / "native.py").write_text(NATIVE)
        (self.root / "supervisor.py").write_text(SUPERVISOR)
        self.sequence = 0

    def request(self, operation):
        self.sequence += 1
        return {"request_id": f"{self.sequence:032x}", "device_id": DEVICE,
                "operation": operation, "arguments": {"key": 3} if operation == "send_key" else {}}

    def wait_file(self, name, timeout=5):
        path, deadline = self.root / name, time.monotonic() + timeout
        while not path.exists():
            if time.monotonic() >= deadline:
                self.fail("The local dummy process did not reach " + name)
            time.sleep(.01)
        return path

    @staticmethod
    def running(pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        status = Path(f"/proc/{pid}/stat")
        if status.exists():
            return status.read_text().rsplit(")", 1)[1].split()[0] != "Z"
        return True

    def stop_tree_if_needed(self):
        for name in ("descendant.pid", "native.pid"):
            path = self.root / name
            if path.exists():
                try:
                    os.kill(int(path.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def worker(self, *, timeout=3, cleanup_failure=False, cleanup_signal=False):
        enrollment = SimpleNamespace(python=sys.executable, storage=str(self.storage),
                                     sdk_path=str(self.sdk), paa_path=str(self.paa))
        worker = _Worker(enrollment, timeout)

        def supervisor(command, **options):
            self.assertIn("--worker", command)
            environment = dict(options["env"], AMITY_CANCELLATION_ROOT=str(self.root))
            if cleanup_failure:
                environment["AMITY_CANCELLATION_FAILURE"] = "1"
            if cleanup_signal:
                environment["AMITY_CANCELLATION_CLEANUP_SIGNAL"] = "1"
            options["env"] = environment
            return REAL_POPEN([sys.executable, str(self.root / "supervisor.py"), *command[4:]], **options)

        self.addCleanup(self.stop_tree_if_needed)
        self.addCleanup(worker.stop)
        with patch("amity_harness.matter.subprocess.Popen", side_effect=supervisor):
            worker.start()
        self.assertEqual(worker.rpc(self.request("open")), {"opened": True})
        self.wait_file("descendant.pid")
        return worker

    def assert_released_and_stopped(self):
        self.assertTrue((self.root / "cleanup-finished").exists())
        for name in ("native.pid", "descendant.pid"):
            pid, deadline = int((self.root / name).read_text()), time.monotonic() + 2
            while self.running(pid) and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertFalse(self.running(pid), name + " still runs")
        with (self.fabric / "accessory.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual((self.root / "launches").read_text().splitlines(), ["native"])

    def test_parent_pipe_eof_stops_child_and_grandchild_before_fabric_unlock(self):
        worker = self.worker()
        worker.process.stdin.close()
        self.wait_file("cleanup-started")
        # Both dummies ignore TERM, so this observes the actual cleanup window.
        self.assertTrue(self.running(int((self.root / "descendant.pid").read_text())))
        with (self.fabric / "accessory.lock").open("a") as lock:
            with self.assertRaises(BlockingIOError):
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual(worker.process.wait(timeout=8), 0)
        self.assert_released_and_stopped()

    def test_sigterm_stops_native_group_before_fabric_unlock(self):
        worker = self.worker()
        worker.stop()
        self.assertIsNone(worker.process)
        self.assert_released_and_stopped()

    def test_signal_at_cleanup_entry_cannot_skip_native_group_stop(self):
        worker = self.worker(cleanup_signal=True)
        worker.process.stdin.close()
        worker.process.wait(timeout=8)
        self.assert_released_and_stopped()

    def test_uncertain_command_timeout_poisoning_never_replays_or_restarts(self):
        worker = self.worker()
        with self.assertRaises((MatterError, TimeoutError)):
            worker.rpc(self.request("send_key"))
        self.assertIsNone(worker.process)
        with self.assertRaises(MatterError):
            worker.rpc(self.request("send_key"))
        self.assert_released_and_stopped()
        commands = [json.loads(line) for line in (self.root / "commands.jsonl").read_text().splitlines()]
        self.assertEqual([row["operation"] for row in commands], ["open", "send_key"])

    def test_cleanup_error_cannot_acknowledge_a_successful_close_or_restart(self):
        worker = self.worker(cleanup_failure=True)
        with self.assertRaises(MatterError):
            worker.rpc(self.request("close"))
        self.assertIsNone(worker.process)
        self.assert_released_and_stopped()
        commands = [json.loads(line) for line in (self.root / "commands.jsonl").read_text().splitlines()]
        self.assertEqual([row["operation"] for row in commands], ["open", "close"])


if __name__ == "__main__":
    unittest.main()
