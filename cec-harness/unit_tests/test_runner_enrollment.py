"""Enrollment gates firmware and scenarios before any test fixtures open."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import ANY, Mock, patch

from amity_harness.config import RunConfig, current_config, use_config
from amity_harness.runner import main, prepare_main, reset_main


class EnrollmentRunnerTests(unittest.TestCase):
    def invoke(self, tests, options=(), **callbacks):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return main(tests, ["--credentials", "/private/main.json", "--no-status-led",
                                *options], **callbacks)

    def test_preparation_precedes_upgrade_and_all_scenarios_once(self):
        calls = []
        with tempfile.TemporaryDirectory() as directory:
            firmware = Path(directory) / "update.bin"
            firmware.write_bytes(b"firmware")
            self.assertEqual(self.invoke(
                {"first": lambda: calls.append("first"), "second": lambda: calls.append("second")},
                ["--test", "all", "--firmware", str(firmware)],
                prepare=lambda: calls.append("enrollment"),
                firmware_upgrade=lambda: calls.append("upgrade")), 0)
        self.assertEqual(calls, ["enrollment", "upgrade", "first", "second"])

    def test_enrollment_only_never_runs_a_functional_scenario(self):
        calls = []
        self.assertEqual(self.invoke({"scenario": lambda: self.fail("scenario ran")},
                                     ["--test", "enrollment"],
                                     prepare=lambda: calls.append("enrollment")), 0)
        self.assertEqual(calls, ["enrollment"])

    def test_failed_enrollment_stops_upgrade_and_tests_and_writes_report(self):
        def fail():
            raise RuntimeError("private commissioning details")

        with tempfile.TemporaryDirectory() as directory:
            firmware = Path(directory) / "update.bin"
            firmware.write_bytes(b"firmware")
            report = Path(directory) / "report.json"
            self.assertEqual(self.invoke(
                {"scenario": lambda: self.fail("scenario ran")},
                ["--test", "all", "--firmware", str(firmware), "--output", str(report)],
                prepare=fail, firmware_upgrade=lambda: self.fail("upgrade ran")), 1)
            result = json.loads(report.read_text())
            self.assertEqual(result["test"], "fail")
            self.assertFalse(result["passed"])
            self.assertNotIn("private commissioning details", report.read_text())

    def test_failed_upgrade_still_stops_scenarios_after_successful_enrollment(self):
        calls = []

        def fail_upgrade():
            calls.append("upgrade")
            raise RuntimeError("upgrade failed")

        with tempfile.TemporaryDirectory() as directory:
            firmware = Path(directory) / "update.bin"
            firmware.write_bytes(b"firmware")
            self.assertEqual(self.invoke(
                {"scenario": lambda: self.fail("scenario ran")},
                ["--firmware", str(firmware)], prepare=lambda: calls.append("enrollment"),
                firmware_upgrade=fail_upgrade), 1)
        self.assertEqual(calls, ["enrollment", "upgrade"])

    def test_pi_settings_configure_automatic_enrollment(self):
        def prepare():
            config = current_config()
            commissioner = config.enrollment_factory()
            self.assertEqual(commissioner.credentials, Path("/private/enrollment.json"))
            self.assertEqual(commissioner.python, sys.executable)
            self.assertEqual(commissioner.timeout, 210)
            self.assertEqual(config.enrollment_timeout, 210)

        self.assertEqual(self.invoke({"scenario": lambda: None},
                                     ["--test", "enrollment",
                                      "--enrollment-credentials", "/private/enrollment.json",
                                      "--enrollment-timeout", "210"],
                                     prepare=prepare), 0)

    def test_actual_entrypoint_always_registers_preparation(self):
        with patch("amity_harness.runner.main", return_value=0) as entry:
            with self.assertRaises(SystemExit):
                runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertIs(entry.call_args.kwargs["prepare"], prepare_main)
        self.assertIs(entry.call_args.kwargs["finish"], reset_main)

    def test_successful_run_resets_after_all_scenarios(self):
        calls = []
        self.assertEqual(self.invoke({"scenario": lambda: calls.append("scenario")},
                                     prepare=lambda: calls.append("enroll"),
                                     finish=lambda: calls.append("reset")), 0)
        self.assertEqual(calls, ["enroll", "scenario", "reset"])

    def test_failed_or_interrupted_run_preserves_main_for_debugging(self):
        for error in (AssertionError("scenario failed"), KeyboardInterrupt()):
            calls = []

            def fail():
                calls.append("scenario")
                raise error

            options = {"prepare": lambda: calls.append("enroll"),
                       "finish": lambda: self.fail("Failed run reset the main")}
            with self.subTest(error=type(error).__name__):
                if isinstance(error, KeyboardInterrupt):
                    with self.assertRaises(KeyboardInterrupt):
                        self.invoke({"scenario": fail}, **options)
                else:
                    self.assertEqual(self.invoke({"scenario": fail}, **options), 1)
                self.assertEqual(calls, ["enroll", "scenario"])

    def test_enrollment_only_resets_after_verification(self):
        calls = []
        self.assertEqual(self.invoke({"scenario": lambda: self.fail("scenario ran")},
                                     ["--test", "enrollment"],
                                     prepare=lambda: calls.append("enroll"),
                                     finish=lambda: calls.append("reset")), 0)
        self.assertEqual(calls, ["enroll", "reset"])

    def test_reset_failure_fails_the_run(self):
        def reset():
            raise RuntimeError("reset not confirmed")

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            self.assertEqual(self.invoke({"scenario": lambda: None}, ["--output", str(report)],
                                         prepare=lambda: None, finish=reset), 1)
            result = json.loads(report.read_text())
            self.assertFalse(result["passed"])
            self.assertEqual(result["tests"][-1]["test"], "reset")

    def test_reset_uses_authenticated_ui_then_verifies_empty_factory_state(self):
        commissioner = Mock()
        with use_config(RunConfig(enrollment_factory=lambda: commissioner)), \
             patch("amity_harness.dut.DUT") as dut_type, \
             patch("amity_harness.enrollment._probe_ready", side_effect=ConnectionRefusedError()):
            dut = dut_type.return_value
            dut.get_system.return_value = {"octopus_id": dut.identity.device_id}
            dut.request.return_value = {"rebooting": True}
            reset_main()
            dut.authenticate.assert_called_once_with()
            dut.request.assert_called_once_with("/api/factory-reset", {"confirm": True})
            commissioner.verify_factory.assert_called_once_with(dut, timeout=ANY)
            self.assertIsNone(dut._token)

    def test_reset_refuses_another_http_identity(self):
        commissioner = Mock()
        with use_config(RunConfig(enrollment_factory=lambda: commissioner)), \
             patch("amity_harness.dut.DUT") as dut_type:
            dut = dut_type.return_value
            dut.get_system.return_value = {"octopus_id": "another-board"}
            with self.assertRaises(AssertionError):
                reset_main()
            dut.request.assert_not_called()
            commissioner.verify_factory.assert_not_called()

    def test_preparation_closes_authentication_after_success_and_failure(self):
        for failure in (False, True):
            with self.subTest(failure=failure), use_config(RunConfig(enrollment_timeout=210)), \
                 patch("amity_harness.dut.DUT") as dut_type, \
                 patch("amity_harness.enrollment.MatterEnrollment") as enrollment_type, \
                 patch("amity_harness.enrollment.ensure_enrolled") as ensure:
                if failure:
                    ensure.side_effect = RuntimeError("failed")
                    with self.assertRaises(RuntimeError):
                        prepare_main()
                else:
                    prepare_main()
                ensure.assert_called_once_with(dut_type.return_value, enrollment_type.return_value,
                                               timeout=210)
                dut_type.return_value.logout.assert_called_once_with()

    def test_enrollment_timeout_rejects_nonfinite_or_nonpositive_values(self):
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                self.invoke({"scenario": lambda: None}, ["--enrollment-timeout", value])


if __name__ == "__main__":
    unittest.main()
