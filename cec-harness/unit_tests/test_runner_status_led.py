"""Run-wide LED lifetime without SSH, GPIO, CEC or DUT access."""

from contextlib import ExitStack, contextmanager, redirect_stderr, redirect_stdout
import io
import unittest
from unittest.mock import patch

from amity_harness.runner import main


class RunnerStatusLedTests(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)

        @contextmanager
        def indicator(**options):
            self.options = options
            self.events.append("led.start")
            try:
                yield
            finally:
                self.events.append("led.stop")

        self.indicator = self.stack.enter_context(patch("amity_harness.runner.run_status_led", side_effect=indicator))
        self.stack.enter_context(redirect_stdout(io.StringIO()))
        self.errors = self.stack.enter_context(redirect_stderr(io.StringIO()))

    def run_cli(self, tests, *options):
        return main(tests, ["--credentials", "/unused/credentials.json", *options])

    def test_entire_suite_and_report_share_one_indicator(self):
        def first():
            self.events.append("first")

        def second():
            self.events.append("second")

        def write_report(*unused):
            self.events.append("report")

        with patch("amity_harness.runner._write_report", side_effect=write_report):
            self.assertEqual(self.run_cli({"first": first, "second": second},
                                         "--test", "all", "--output", "/unused/result.json"), 0)
        self.assertEqual(self.events, ["led.start", "first", "second", "report", "led.stop"])
        self.indicator.assert_called_once()
        self.assertIn("run.report", self.errors.getvalue().splitlines()[-1])

    def test_failed_scenario_restores_indicator(self):
        def failed():
            self.events.append("failed")
            raise AssertionError("expected")

        self.assertEqual(self.run_cli(failed), 1)
        self.assertEqual(self.events, ["led.start", "failed", "led.stop"])

    def test_interruption_and_report_failure_restore_indicator(self):
        for failure in (KeyboardInterrupt, OSError):
            with self.subTest(failure=failure):
                self.events.clear()

                def scenario():
                    self.events.append("scenario")
                    if failure is KeyboardInterrupt:
                        raise KeyboardInterrupt

                with patch("amity_harness.runner._write_report", side_effect=OSError), \
                        self.assertRaises(failure):
                    self.run_cli(scenario, "--output", "/unused/result.json")
                self.assertEqual(self.events, ["led.start", "scenario", "led.stop"])

    def test_remote_options_reach_indicator(self):
        self.assertEqual(self.run_cli(lambda: None, "--harness-host", "pi@bench.example",
                                     "--harness-root", "/bench/harness root", "--ssh-port", "2222"), 0)
        self.assertEqual(self.options, {"host": "pi@bench.example", "root": "/bench/harness root", "port": 2222})

    def test_opt_out_never_starts_indicator(self):
        self.assertEqual(self.run_cli(lambda: None, "--no-status-led"), 0)
        self.indicator.assert_not_called()


if __name__ == "__main__":
    unittest.main()
