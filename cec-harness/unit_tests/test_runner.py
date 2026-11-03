"""Generic functional runner configuration, cleanup and private evidence."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

from amity_harness import DUT, RunConfig, TestBed, TvComponent
from amity_harness.config import current_config, use_config
from amity_harness.runner import main, run_test
from amity_harness.gpio import SSHPinctrlGPIO
from amity_harness.trace import owner_assertion


class RunnerTests(unittest.TestCase):
    def execute(self, fail=False):
        observed = {}

        def authenticate(dut):
            dut._token = "private-session-token"

        def logout(dut):
            observed["logged_out"] = dut.name
            dut._token = None

        def scenario():
            dut = DUT("amity-dut")
            observed["dut"] = dut
            with TestBed(dut):
                observed["active_url"] = dut.url
                if fail:
                    raise AssertionError("expected scenario failure")

        config = RunConfig(default_credentials=Path("/private/credential-file.json"),
                           urls={"amity-dut": "https://bench.example"})
        original = current_config()
        with patch.object(DUT, "authenticate", authenticate), patch.object(DUT, "logout", logout), \
             patch.object(DUT, "get_system", lambda dut: {"octopus_id": dut.identity.device_id}):
            report = run_test(scenario, config)
        self.assertIs(current_config(), original)
        self.assertIsNone(observed["dut"]._testbed)
        self.assertIsNone(observed["dut"]._token)
        self.assertEqual(observed["logged_out"], "amity-dut")
        self.assertEqual(observed["active_url"], "https://bench.example")
        serialized = json.dumps(report)
        self.assertNotIn("private-session-token", serialized)
        self.assertNotIn("credential-file", serialized)
        self.assertEqual(len(report["testbeds"]), 1)
        return report

    def test_runner_collects_resources_without_a_test_return_value(self):
        report = self.execute()
        self.assertTrue(report["passed"])
        self.assertTrue(report["testbeds"][0]["passed"])

    def test_scenario_failure_still_cleans_up_and_retains_evidence(self):
        report = self.execute(fail=True)
        self.assertFalse(report["passed"])
        self.assertEqual(report["error"], "AssertionError")
        self.assertEqual(report["failure"]["error_type"], "AssertionError")
        self.assertNotIn("expected scenario failure", json.dumps(report))
        self.assertFalse(report["testbeds"][0]["passed"])

    def test_actual_bed_cleanup_retains_controlled_primary_failure_in_report_chain(self):
        def fail_cleanup():
            raise RuntimeError("private-cleanup-body-token")

        def scenario():
            with TestBed(DUT("amity-dut")) as bed:
                bed.add_cleanup(fail_cleanup)
                raise owner_assertion("member_status")

        with patch.object(DUT, "authenticate", lambda dut: None), \
             patch.object(DUT, "logout", lambda dut: None), \
             patch.object(DUT, "get_system", lambda dut: {"octopus_id": dut.identity.device_id}):
            report = run_test(scenario, RunConfig())
        self.assertEqual(report["error"], "TestBedCleanupError")
        cause = report["failure"]["causes"][0]
        self.assertEqual(cause["error_type"], "AssertionError")
        self.assertEqual(cause["reason"], "member_status")
        self.assertEqual(cause["stack"][-1]["function"], "scenario")
        self.assertEqual(report["testbeds"][0]["failure"]["reason"], "member_status")
        self.assertEqual(report["testbeds"][0]["cleanup_errors"], ["RuntimeError"])
        self.assertNotIn("private-cleanup-body-token", json.dumps(report))

    def test_shared_cli_supports_per_dut_settings_and_private_reports(self):
        def scenario():
            pass

        def capture(test, config):
            self.assertIs(test, scenario)
            self.assertEqual(config.credentials_for("amity-dut-2"), Path("/private/member.json"))
            self.assertEqual(config.url_for("amity-dut-2"), "https://member.example")
            self.assertEqual(config.wiring_path, Path("/bench/wiring.md"))
            self.assertTrue(config.insecure)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.json"
            with patch("amity_harness.runner.run_test", side_effect=capture), redirect_stdout(io.StringIO()):
                code = main(scenario, ["--dut-credentials", "amity-dut-2=/private/member.json",
                                       "--dut-url", "amity-dut-2=https://member.example",
                                       "--wiring", "/bench/wiring.md", "--insecure",
                                       "--output", str(output)])
            self.assertEqual(code, 0)
            self.assertTrue(json.loads(output.read_text())["passed"])
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)

    def test_named_scenarios_and_suite_report_keep_failure_evidence(self):
        called = []

        def scan():
            called.append("scan")

        def activity():
            called.append("activity")
            raise AssertionError("missing physical standby command")

        def source_theft():
            called.append("source_theft")

        def add_amity():
            called.append("add_amity")

        scenarios = {"hdmi_scan": scan, "activity": activity,
                     "source_theft": source_theft, "add_amity": add_amity}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.json"
            options = ["--credentials", "/private/credentials.json", "--output", str(output)]
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main(scenarios, options), 0)
            self.assertEqual(called, ["scan"])
            called.clear()
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main(scenarios, [*options, "--test", "source_theft"]), 0)
            self.assertEqual(called, ["source_theft"])
            self.assertTrue(json.loads(output.read_text())["passed"])
            called.clear()
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main(scenarios, [*options, "--test", "add_amity"]), 0)
            self.assertEqual(called, ["add_amity"])
            called.clear()
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(main(scenarios, [*options, "--test", "all"]), 1)
            report = json.loads(output.read_text())
            self.assertEqual(called, ["scan", "activity", "source_theft", "add_amity"])
            self.assertFalse(report["passed"])
            self.assertTrue(report["tests"][0]["passed"])
            self.assertEqual(report["tests"][1]["error"], "AssertionError")
            self.assertNotIn("physical standby command", json.dumps(report))
            self.assertTrue(report["tests"][2]["passed"])
            self.assertTrue(report["tests"][3]["passed"])

    def test_harness_host_selects_remote_gpio_and_cec_without_performing_io(self):
        def scenario():
            pass

        def capture(test, config):
            gpio = config.gpio_factory()
            self.assertIsInstance(gpio, SSHPinctrlGPIO)
            self.assertEqual((gpio.host, gpio.root, gpio.port),
                             ("pi@bench.example", "/bench/harness root", 2222))
            self.assertIn("backend_factory", config.session_factory.keywords)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        with patch("amity_harness.runner.run_test", side_effect=capture), \
                patch("amity_harness.runner.run_status_led"), redirect_stdout(io.StringIO()):
            self.assertEqual(main(scenario, ["--credentials", "/private/main.json",
                                            "--harness-host", "pi@bench.example",
                                            "--harness-root", "/bench/harness root", "--ssh-port", "2222"]), 0)

    def test_cli_keeps_cec_assertions_independent_from_long_operation_timeouts(self):
        def scenario():
            pass

        for options, operation_timeout, expected in (([], 180, 5), (["--timeout", "300"], 300, 5),
                (["--cec-timeout", "0.25"], 180, 0.25), (["--timeout", "300", "--cec-timeout", "0.25"], 300, 0.25)):
            with self.subTest(options=options):
                def capture(test, config):
                    self.assertEqual(config.timeout, operation_timeout)
                    self.assertEqual(config.cec_timeout, expected)
                    with use_config(config):
                        dut = DUT("amity-dut")
                        component = TvComponent(dut.tentacle(0).front, "CEC timeout TV")
                        self.assertEqual(component._power_timeout, expected)
                    return {"test": test.__name__, "passed": True, "testbeds": []}

                with patch("amity_harness.runner.run_test", side_effect=capture), \
                        redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.assertEqual(main(scenario, ["--credentials", "/private/main.json", *options]), 0)

    def test_invalid_cec_timeout_is_rejected_before_running_a_scenario(self):
        for value in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(value=value), patch("amity_harness.runner.run_test") as execute, \
                    redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
                main(lambda: None, ["--credentials", "/private/main.json", f"--cec-timeout={value}"])
            self.assertEqual(exited.exception.code, 2)
            execute.assert_not_called()

    def test_activity_group_retains_independent_failure_cleanup_and_evidence(self):
        called, logged_out, duts = [], [], []

        def activity_power():
            dut = DUT("amity-dut")
            duts.append(dut)
            with TestBed(dut):
                called.append("power")
                raise AssertionError("missing physical power command")

        def activity_switch_input():
            dut = DUT("amity-dut")
            duts.append(dut)
            with TestBed(dut):
                called.append("switch")

        def logout(dut):
            logged_out.append(dut.name)
            dut._token = None

        output, errors = io.StringIO(), io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "activity.json"
            with patch.object(DUT, "authenticate", lambda dut: setattr(dut, "_token", "private-token")), \
                    patch.object(DUT, "logout", logout), \
                    patch.object(DUT, "get_system", lambda dut: {"octopus_id": dut.identity.device_id}), \
                    redirect_stdout(output), redirect_stderr(errors):
                code = main({"activity": [activity_power, activity_switch_input]},
                            ["--test", "activity", "--credentials", "/private/main.json", "--output", str(evidence)])
            report = json.loads(evidence.read_text())
        self.assertEqual(code, 1)
        self.assertEqual(called, ["power", "switch"])
        self.assertEqual(logged_out, ["amity-dut", "amity-dut"])
        self.assertTrue(all(dut._testbed is None and dut._token is None for dut in duts))
        self.assertFalse(report["passed"])
        self.assertEqual([result["test"] for result in report["tests"]], ["activity_power", "activity_switch_input"])
        self.assertEqual([result["passed"] for result in report["tests"]], [False, True])
        self.assertEqual([len(result["testbeds"]) for result in report["tests"]], [1, 1])
        self.assertFalse(report["tests"][0]["testbeds"][0]["passed"])
        self.assertTrue(report["tests"][1]["testbeds"][0]["passed"])
        self.assertEqual(report["tests"][0]["error"], "AssertionError")
        self.assertNotIn("missing physical power command", json.dumps(report))
        self.assertIn("PASS: activity_switch_input", output.getvalue())
        self.assertIn("FAIL: AssertionError", errors.getvalue())
        self.assertIn("run.report", errors.getvalue())
        self.assertIn(str(evidence.resolve()), errors.getvalue())
        self.assertNotIn("private-token", json.dumps(report))

    def test_actual_all_registration_runs_all_activity_subtests_once(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "all.json"
            arguments = ["run.py", "--test", "all", "--credentials", "/private/main.json", "--output", str(evidence)]
            with patch("sys.argv", arguments), patch("amity_harness.runner.run_test", side_effect=capture), \
                    redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as exited:
                runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
            self.assertEqual(exited.exception.code, 0)
            report = json.loads(evidence.read_text())
        expected = ["test_hdmi_scan", "test_activity", "test_activity_switch_input",
                    "test_activity_unselected_equipment_announcement",
                    "test_activity_keys", "test_activity_keys_secondary",
                    "test_activity_keys_disconnected", "test_activity_keys_disconnected_secondary",
                    "test_source_theft", "test_add_amity",
                    "test_consortium", "test_consortium_three_duts", "test_consortium_four_duts",
                    "test_dual_display", "test_source_theft_isolated", "test_qualified_names", "test_missing_equipment",
                    "test_removal_cascade", "test_default_physical_address_local",
                    "test_default_physical_address_local_scanned", "test_default_physical_address_remote",
                    "test_default_physical_address_remote_scanned",
                    "test_ble_keyboard_local", "test_ble_keyboard_remote"]
        self.assertEqual(called, expected)
        self.assertEqual([result["test"] for result in report["tests"]], expected)
        self.assertTrue(report["passed"])

    def test_default_physical_address_selects_all_four_paths_without_other_scenarios(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        arguments = ["run.py", "--test", "default_physical_address", "--credentials", "/private/main.json"]
        with patch("sys.argv", arguments), patch("amity_harness.runner.run_test", side_effect=capture), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertEqual(exited.exception.code, 0)
        self.assertEqual(called, ["test_default_physical_address_local", "test_default_physical_address_local_scanned",
                                 "test_default_physical_address_remote", "test_default_physical_address_remote_scanned"])

    def test_removal_cascade_can_be_selected_without_other_scenarios(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        arguments = ["run.py", "--test", "removal_cascade", "--credentials", "/private/main.json"]
        with patch("sys.argv", arguments), patch("amity_harness.runner.run_test", side_effect=capture), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertEqual(exited.exception.code, 0)
        self.assertEqual(called, ["test_removal_cascade"])

    def test_three_dut_consortium_can_be_selected_without_other_scenarios(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        arguments = ["run.py", "--test", "consortium_three_duts", "--credentials", "/private/main.json"]
        with patch("sys.argv", arguments), patch("amity_harness.runner.run_test", side_effect=capture), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertEqual(exited.exception.code, 0)
        self.assertEqual(called, ["test_consortium_three_duts"])

    def test_four_dut_consortium_can_be_selected_without_other_scenarios(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        arguments = ["run.py", "--test", "consortium_four_duts", "--credentials", "/private/main.json"]
        with patch("sys.argv", arguments), patch("amity_harness.runner.run_test", side_effect=capture), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertEqual(exited.exception.code, 0)
        self.assertEqual(called, ["test_consortium_four_duts"])

    def test_dual_display_can_be_selected_without_other_scenarios(self):
        called = []

        def capture(test, config):
            called.append(test.__name__)
            return {"test": test.__name__, "passed": True, "testbeds": []}

        arguments = ["run.py", "--test", "dual_display", "--credentials", "/private/main.json"]
        with patch("sys.argv", arguments), patch("amity_harness.runner.run_test", side_effect=capture), \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exited:
            runpy.run_path(str(Path(__file__).resolve().parents[1] / "run.py"), run_name="__main__")
        self.assertEqual(exited.exception.code, 0)
        self.assertEqual(called, ["test_dual_display"])

    def test_invalid_scenario_groups_are_rejected_before_execution(self):
        def scenario():
            self.fail("Invalid registration must not run a test")

        for group in ([], (), None, 1, [scenario, None], (scenario, [scenario])):
            with self.subTest(group=group), self.assertRaisesRegex(ValueError, "nonempty groups of callable tests"):
                main({"activity": group}, [])


if __name__ == "__main__":
    unittest.main()
