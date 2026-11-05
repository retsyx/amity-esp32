"""Run-indicator ownership tests using fake LEDs, pipes and temporary locks."""

from contextlib import redirect_stdout
import copy
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from amity_harness.status_led import (
    _LEDs, _SharedLED, _close, _command, _message, _state, main, run_status_led,
)


ORIGINAL = {"ACT": {"trigger": "mmc0", "brightness": "0"},
            "PWR": {"trigger": "none", "brightness": "0"}}


class LEDs:
    def __init__(self, failure=False):
        self.values = copy.deepcopy(ORIGINAL)
        self.calls = []
        self.failure = failure

    def snapshot(self):
        self.calls.append("snapshot")
        return copy.deepcopy(self.values)

    def running(self):
        self.calls.append("running")
        self.values = {"ACT": {"trigger": "none", "brightness": "0"},
                       "PWR": {"trigger": "timer", "brightness": "1"}}
        if self.failure:
            raise PermissionError("private failure text")

    def restore(self, values):
        self.calls.append("restore")
        self.values = copy.deepcopy(values)


class StatusLEDTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name) / "state"
        self.addCleanup(self.temporary.cleanup)

    def shared(self, leds):
        owner = _SharedLED(leds, self.directory)
        self.addCleanup(owner.close)
        return owner

    def test_first_run_blinks_and_last_of_overlapping_runs_restores_exact_original(self):
        leds = LEDs()
        first, second, third = (self.shared(leds) for _ in range(3))
        first.start()
        second.start()
        third.start()
        self.assertEqual(leds.calls, ["snapshot", "running"])
        self.assertEqual(leds.values["PWR"]["trigger"], "timer")
        second.close()
        first.close()
        self.assertEqual(leds.calls, ["snapshot", "running"])
        third.close()
        self.assertEqual(leds.values, ORIGINAL)
        self.assertEqual(leds.calls, ["snapshot", "running", "restore"])
        self.assertFalse((self.directory / "original.json").exists())

    def test_later_run_takes_fresh_snapshot_after_original_run_finished(self):
        leds = LEDs()
        first = self.shared(leds)
        first.start()
        first.close()
        leds.values["PWR"] = {"trigger": "default-on", "brightness": "1"}
        changed = copy.deepcopy(leds.values)
        second = self.shared(leds)
        second.start()
        second.close()
        self.assertEqual(leds.values, changed)

    def test_killed_worker_state_is_recovered_before_starting_another_run(self):
        leds = LEDs()
        first = self.shared(leds)
        first.start()
        # Kernel closes these descriptors after SIGKILL; the saved state survives.
        for attribute in ("membership", "coordination", "directory_fd"):
            os.close(getattr(first, attribute))
            setattr(first, attribute, None)
        first.joined = False
        second = self.shared(leds)
        second.start()
        self.assertEqual(leds.calls, ["snapshot", "running", "restore", "snapshot", "running"])
        second.close()
        self.assertEqual(leds.values, ORIGINAL)

    def test_partial_setup_failure_restores_both_leds(self):
        leds = LEDs(failure=True)
        owner = self.shared(leds)
        with self.assertRaises(PermissionError):
            owner.start()
        owner.close()
        self.assertEqual(leds.values, ORIGINAL)
        self.assertEqual(leds.calls, ["snapshot", "running", "restore"])
        self.assertFalse((self.directory / "original.json").exists())

    def test_unsafe_state_directory_and_symlink_lock_are_rejected(self):
        self.directory.mkdir(mode=0o755)
        with self.assertRaises(PermissionError):
            self.shared(LEDs()).start()
        self.directory.chmod(0o700)
        target = Path(self.temporary.name) / "target"
        target.write_text("unchanged")
        (self.directory / "coordination.lock").symlink_to(target)
        with self.assertRaises(OSError):
            self.shared(LEDs()).start()
        self.assertEqual(target.read_text(), "unchanged")

    def test_corrupt_saved_state_does_not_change_leds(self):
        self.directory.mkdir(mode=0o700)
        state = self.directory / "original.json"
        state.write_text('{"PWR":{"path":"/private/file"}}')
        state.chmod(0o600)
        leds = LEDs()
        owner = self.shared(leds)
        with self.assertRaises(ValueError):
            owner.start()
        # Failure restoration also rejects corrupt metadata without hardware writes.
        with self.assertRaises(ValueError):
            owner.close()
        self.assertEqual(leds.calls, [])

    def make_sysfs(self):
        root = Path(self.temporary.name) / "leds"
        for name, settings in ORIGINAL.items():
            directory = root / name
            directory.mkdir(parents=True)
            for field, value in settings.items():
                if field == "trigger":
                    value = "none timer heartbeat mmc0".replace(value, "[" + value + "]")
                (directory / field).write_text(value)
            for field in ("delay_on", "delay_off"):
                (directory / field).write_text("50")
        return root

    def test_sysfs_snapshot_blinks_red_and_restores_triggers_and_brightness(self):
        root = self.make_sysfs()
        leds = _LEDs(root)
        original = leds.snapshot()
        self.assertEqual(original, ORIGINAL)
        leds.running()
        self.assertEqual((root / "ACT/trigger").read_text().strip(), "none")
        self.assertEqual((root / "ACT/brightness").read_text().strip(), "0")
        self.assertEqual((root / "PWR/trigger").read_text().strip(), "timer")
        self.assertEqual((root / "PWR/delay_on").read_text().strip(), "300")
        self.assertEqual((root / "PWR/delay_off").read_text().strip(), "300")
        leds.restore(original)
        for name, settings in ORIGINAL.items():
            for field, value in settings.items():
                self.assertEqual((root / name / field).read_text().strip(), value)

    def test_timer_snapshot_preserves_previous_delays(self):
        root = self.make_sysfs()
        (root / "PWR/trigger").write_text("none [timer] heartbeat")
        leds = _LEDs(root)
        original = leds.snapshot()
        leds.running()
        leds.restore(original)
        self.assertEqual((root / "PWR/delay_on").read_text().strip(), "50")
        self.assertEqual((root / "PWR/delay_off").read_text().strip(), "50")

    def test_privileged_write_is_bounded_noninteractive_and_fixed_to_hardware_paths(self):
        with patch("pathlib.Path.write_text", side_effect=PermissionError), \
                patch("amity_harness.status_led.subprocess.run") as run:
            _LEDs()._write("PWR", "trigger", "timer")
        run.assert_called_once_with(["sudo", "-n", "tee", "/sys/class/leds/PWR/trigger"],
                                    input="timer\n", text=True, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, check=True, timeout=3)
        with patch("pathlib.Path.write_text", side_effect=PermissionError), \
                patch("amity_harness.status_led.subprocess.run") as run, self.assertRaises(PermissionError):
            _LEDs(Path(self.temporary.name))._write("PWR", "trigger", "timer")
        run.assert_not_called()

    def test_invalid_values_and_saved_state_cannot_supply_privileged_paths_or_commands(self):
        leds = _LEDs()
        for name, field, value in (("../ACT", "trigger", "none"), ("PWR", "../trigger", "none"),
                                   ("PWR", "trigger", "timer; id"), ("PWR", "brightness", "-1"),
                                   ("PWR", "delay_on", "$(id)")):
            with self.subTest(name=name, field=field, value=value), self.assertRaises(ValueError):
                leds._write(name, field, value)
        for change in ({}, {**ORIGINAL, "other": {}},
                       {**ORIGINAL, "PWR": {"trigger": "none", "brightness": True}},
                       {**ORIGINAL, "PWR": {"trigger": "../../x", "brightness": "0"}}):
            with self.assertRaises(ValueError):
                _state(change)

    def test_helper_process_restores_on_pipe_eof_without_stop_command(self):
        root = self.make_sysfs()
        code = ("from amity_harness.status_led import _LEDs, _SharedLED; import sys; "
                "owner = _SharedLED(_LEDs(sys.argv[1]), sys.argv[2]); owner.start(); "
                "print('ready', flush=True); sys.stdin.buffer.read(); owner.close()")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
        child = subprocess.Popen([sys.executable, "-u", "-c", code, str(root), str(self.directory)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=environment)
        self.addCleanup(lambda: child.poll() is None and child.kill())
        self.assertEqual(child.stdout.readline(), b"ready\n")
        self.assertEqual((root / "PWR/trigger").read_text().strip(), "timer")
        child.stdin.close()
        child.stdin = None
        output, error = child.communicate(timeout=5)
        self.assertEqual(child.returncode, 0, error)
        self.assertEqual(output, b"")
        self.assertEqual((root / "PWR/trigger").read_text().strip(), "none")
        self.assertEqual((root / "ACT/trigger").read_text().strip(), "mmc0")

    def test_command_uses_current_python_and_one_unprivileged_local_worker(self):
        self.assertEqual(_command(), [sys.executable, "-u", "-m", "amity_harness.status_led", "--worker"])

    def process(self):
        process = Mock()
        process.returncode = 0
        process.communicate.return_value = (b"", b"")
        return process

    def test_context_covers_run_exception_and_always_closes_the_worker(self):
        process = self.process()
        with patch("amity_harness.status_led.subprocess.Popen", return_value=process) as start, \
                patch("amity_harness.status_led._message", return_value=True), \
                self.assertRaisesRegex(RuntimeError, "test failed"):
            with run_status_led() as active:
                self.assertTrue(active)
                raise RuntimeError("test failed")
        self.assertEqual(start.call_args.kwargs["stdin"], subprocess.PIPE)
        self.assertEqual(start.call_args.kwargs["stdout"], subprocess.PIPE)
        process.communicate.assert_called_once_with(timeout=40)
        self.assertIsNone(process.stdin)

    def test_unavailable_helper_and_startup_failure_never_fail_scenario(self):
        for failure in (FileNotFoundError("secret path"), TimeoutError("secret host")):
            process = self.process()
            with patch("amity_harness.status_led.subprocess.Popen", return_value=process), \
                    patch("amity_harness.status_led._message", side_effect=failure), \
                    patch("amity_harness.status_led._event") as event:
                with run_status_led() as active:
                    self.assertFalse(active)
                    process.communicate.assert_called_once()
                self.assertNotIn("secret", str(event.call_args_list))
            process.communicate.assert_called_once()
        with patch("amity_harness.status_led.subprocess.Popen", side_effect=OSError):
            with run_status_led() as active:
                self.assertFalse(active)

    def test_stalled_worker_gets_bounded_terminate_then_kill(self):
        process = self.process()
        process.communicate.side_effect = [subprocess.TimeoutExpired("helper", 40),
                                          subprocess.TimeoutExpired("helper", 5), (b"", b"")]
        _close(process)
        process.terminate.assert_called_once()
        process.kill.assert_called_once()
        self.assertEqual([call.kwargs["timeout"] for call in process.communicate.call_args_list], [40, 5, 5])

    def test_cleanup_failure_is_nonfatal_and_preserves_scenario_failure(self):
        process = self.process()
        with patch("amity_harness.status_led.subprocess.Popen", return_value=process), \
                patch("amity_harness.status_led._message", return_value=True), \
                patch("amity_harness.status_led._close", side_effect=OSError("private cleanup text")), \
                patch("amity_harness.status_led._event") as event, \
                self.assertRaisesRegex(RuntimeError, "original failure"):
            with run_status_led():
                raise RuntimeError("original failure")
        self.assertNotIn("private cleanup text", str(event.call_args_list))

    def test_message_rejects_eof_malformed_and_wrong_protocol(self):
        for data, error in ((b"", EOFError), (b"not json\n", ValueError),
                            (b'{"protocol_version":true,"active":true}\n', ValueError),
                            (b'{"protocol_version":1,"active":"yes"}\n', ValueError),
                            (b'{"protocol_version":1,"active":true,"extra":0}\n', ValueError)):
            read_fd, write_fd = os.pipe()
            with os.fdopen(read_fd, "rb", buffering=0) as stream:
                os.write(write_fd, data)
                os.close(write_fd)
                with self.subTest(data=data), self.assertRaises(error):
                    _message(stream, 1)

    def test_worker_signal_and_eof_cleanup_restore_handlers(self):
        for interrupted in (False, True):
            shared, output = Mock(), io.StringIO()
            stdin = SimpleNamespace(buffer=io.BytesIO())
            if interrupted:
                stdin.buffer = Mock()

                def interrupt(size):
                    signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)

                stdin.buffer.read.side_effect = interrupt
            handlers = {signum: signal.getsignal(signum)
                        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
            with patch("amity_harness.status_led._SharedLED", return_value=shared), \
                    patch("amity_harness.status_led.sys.stdin", stdin), redirect_stdout(output):
                self.assertEqual(main(["--worker"]), 0)
            self.assertEqual(json.loads(output.getvalue()), {"protocol_version": 1, "active": True})
            shared.start.assert_called_once()
            shared.close.assert_called_once()
            self.assertEqual(handlers, {signum: signal.getsignal(signum) for signum in handlers})

    def test_worker_unavailable_still_cleans_up_and_reports_inactive(self):
        shared, output = Mock(), io.StringIO()
        shared.start.side_effect = PermissionError("not shared")
        with patch("amity_harness.status_led._SharedLED", return_value=shared), redirect_stdout(output):
            self.assertEqual(main(["--worker"]), 0)
        self.assertEqual(json.loads(output.getvalue()), {"protocol_version": 1, "active": False})
        shared.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
