"""Physical-hold transport tests without GPIO, SSH or DUT network access."""

from contextlib import redirect_stdout
import io
import json
import shlex
import signal
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from amity_harness.config import RunConfig, use_config
from amity_harness.dut import DUT, PinctrlGPIO
from amity_harness.gpio import SSHPinctrlGPIO, hold_low, main
from unit_tests.harness_pins import reset_gpio


class GPIO:
    def __init__(self, failure=None):
        self.calls = []
        self.failure = failure

    def low(self, pin):
        self.calls.append(("low", pin))
        if self.failure is not None:
            raise self.failure

    def release(self, pin):
        self.calls.append(("release", pin))


class GPIOTransportTests(unittest.TestCase):
    def setUp(self):
        self.pin = reset_gpio("amity-dut-2")

    def test_local_hold_times_and_releases_in_same_process_and_preserves_factory_gesture(self):
        for duration in (3.5, 10):
            with self.subTest(duration=duration):
                gpio = GPIO()
                previous = {signum: signal.getsignal(signum)
                            for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM)}
                with patch("amity_harness.gpio.time.sleep") as sleep:
                    result = hold_low(gpio, self.pin, duration)
                sleep.assert_called_once_with(float(duration))
                self.assertEqual(gpio.calls, [("low", self.pin), ("release", self.pin)])
                self.assertEqual(result, {"gpio": self.pin, "seconds": float(duration), "released": True})
                self.assertEqual(previous, {signum: signal.getsignal(signum) for signum in previous})
                self.assertEqual(signal.getitimer(signal.ITIMER_REAL), (0.0, 0.0))

    def test_interruption_or_safety_deadline_still_releases_and_restores_handlers(self):
        for signum, error in ((signal.SIGHUP, InterruptedError), (signal.SIGTERM, InterruptedError),
                              (signal.SIGALRM, TimeoutError)):
            with self.subTest(signum=signum):
                gpio = GPIO()
                original = signal.getsignal(signum)

                def interrupt(seconds):
                    signal.getsignal(signum)(signum, None)

                with patch("amity_harness.gpio.time.sleep", side_effect=interrupt), self.assertRaises(error):
                    hold_low(gpio, self.pin, 3.5)
                self.assertEqual(gpio.calls, [("low", self.pin), ("release", self.pin)])
                self.assertIs(signal.getsignal(signum), original)

    def test_partial_low_failure_still_releases(self):
        gpio = GPIO(OSError("pinctrl interrupted after changing its latch"))
        with self.assertRaises(OSError):
            hold_low(gpio, self.pin, 3.5)
        self.assertEqual(gpio.calls, [("low", self.pin), ("release", self.pin)])

    def test_second_signal_cannot_interrupt_the_release_command(self):
        gpio = GPIO()
        completed = []
        original_mask = signal.pthread_sigmask(signal.SIG_BLOCK, set())

        def interrupt(seconds):
            signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)

        def release(pin):
            gpio.calls.append(("release", pin))
            blocked = signal.pthread_sigmask(signal.SIG_BLOCK, set())
            self.assertTrue({signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM} <= blocked)
            signal.raise_signal(signal.SIGTERM)
            completed.append(True)

        gpio.release = release
        with patch("amity_harness.gpio.time.sleep", side_effect=interrupt), self.assertRaises(InterruptedError):
            hold_low(gpio, self.pin, 3.5)
        self.assertEqual(completed, [True])
        self.assertEqual(gpio.calls, [("low", self.pin), ("release", self.pin)])
        self.assertEqual(signal.pthread_sigmask(signal.SIG_BLOCK, set()), original_mask)

    def test_invalid_gpio_and_duration_reject_before_gpio_or_ssh(self):
        gpio = GPIO()
        for pin in (-1, 28, True, str(self.pin)):
            with self.subTest(pin=pin), self.assertRaises(ValueError):
                hold_low(gpio, pin, 3.5)
        for seconds in (0, -1, True, float("nan"), float("inf"), 9, 61):
            with self.subTest(seconds=seconds), self.assertRaises(ValueError):
                hold_low(gpio, self.pin, seconds)
        self.assertFalse(gpio.calls)

    def test_existing_alarm_or_nonmain_thread_rejects_before_drive(self):
        gpio = GPIO()
        with patch("amity_harness.gpio.signal.getitimer", return_value=(1, 0)), self.assertRaises(RuntimeError):
            hold_low(gpio, self.pin, 3.5)
        with patch("amity_harness.gpio.threading.current_thread", return_value=object()), self.assertRaises(RuntimeError):
            hold_low(gpio, self.pin, 3.5)
        self.assertFalse(gpio.calls)

    def test_dut_uses_runner_gpio_factory_and_explicit_injection_still_overrides_it(self):
        calls = []
        gpio = SimpleNamespace(hold_low=lambda pin, duration: calls.append((pin, duration)))
        with use_config(RunConfig(gpio_factory=lambda: gpio)):
            candidate = DUT("amity-dut-2")
            candidate.press_fw_reset(3.5)
            self.assertEqual(calls, [(candidate.wiring.reset_gpio(candidate.name), 3.5)])
            explicit = DUT("amity-dut-2", gpio=object())
            self.assertIsNot(explicit.gpio, gpio)
        self.assertIsInstance(DUT("amity-dut-2").gpio, PinctrlGPIO)

    def runner(self, change=None, failure=None):
        calls = []

        def run(command, **options):
            calls.append((command, options))
            if failure:
                raise failure
            remote = shlex.split(command[-1])
            result = {"protocol_version": 1, "request_id": remote[remote.index("--request-id") + 1],
                      "gpio": int(remote[remote.index("--gpio") + 1]),
                      "seconds": float(remote[remote.index("--seconds") + 1]), "released": True}
            if change:
                change(result)
            return SimpleNamespace(stdout=json.dumps(result))

        return run, calls

    def test_ssh_transmits_one_complete_hold_with_quoted_path_and_bounded_timeout(self):
        runner, calls = self.runner()
        root = "/bench/harness root/$(literal)"
        gpio = SSHPinctrlGPIO(host="pi@bench.example", root=root, port=2222, runner=runner)
        result = gpio.hold_low(self.pin, 3.5)
        self.assertEqual(result, {"gpio": self.pin, "seconds": 3.5, "released": True})
        self.assertEqual(len(calls), 1)
        command, options = calls[0]
        self.assertEqual(command[0], "ssh")
        self.assertIn("StrictHostKeyChecking=yes", command)
        self.assertIn("BatchMode=yes", command)
        self.assertEqual(command[-2], "pi@bench.example")
        remote = shlex.split(command[-1])
        self.assertEqual(remote[:2], ["env", "PYTHONPATH=" + root])
        self.assertEqual(remote[remote.index("--gpio") + 1], str(self.pin))
        self.assertEqual(options["timeout"], 23.5)
        self.assertTrue(options["check"])

    def test_ssh_requires_exact_release_acknowledgment_and_never_retries(self):
        for change in (lambda r: r.update(released=False), lambda r: r.update(request_id="f" * 32),
                       lambda r: r.update(gpio=-1), lambda r: r.update(seconds=True),
                       lambda r: r.update(protocol_version=True)):
            with self.subTest(change=change):
                runner, calls = self.runner(change)
                with self.assertRaises((ValueError, RuntimeError)):
                    SSHPinctrlGPIO(runner=runner).hold_low(self.pin, 3.5)
                self.assertEqual(len(calls), 1)
        for error in (subprocess.TimeoutExpired(["ssh"], 23.5), subprocess.CalledProcessError(1, ["ssh"])):
            runner, calls = self.runner(failure=error)
            with self.assertRaises(type(error)):
                SSHPinctrlGPIO(runner=runner).hold_low(self.pin, 3.5)
            self.assertEqual(len(calls), 1)

    def test_worker_validates_input_before_gpio_and_reports_only_finished_release(self):
        gpio, output = GPIO(), io.StringIO()
        args = ["--gpio", str(self.pin), "--seconds", "3.5", "--request-id", "a" * 32]
        with patch("amity_harness.gpio.time.sleep"), redirect_stdout(output):
            self.assertEqual(main(args, backend=gpio), 0)
        self.assertEqual(json.loads(output.getvalue()), {"protocol_version": 1, "request_id": "a" * 32,
                                                       "gpio": self.pin, "seconds": 3.5, "released": True})
        self.assertEqual(gpio.calls, [("low", self.pin), ("release", self.pin)])
        gpio.calls.clear()
        with self.assertRaises(ValueError):
            main(["--gpio", str(self.pin), "--seconds", "9", "--request-id", "a" * 32], backend=gpio)
        self.assertFalse(gpio.calls)


if __name__ == "__main__":
    unittest.main()
