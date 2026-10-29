"""Generic functional runner configuration, cleanup and private evidence."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from amity_harness import DUT, RunConfig, TestBed
from amity_harness.config import current_config
from amity_harness.runner import main, run_test


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
        self.assertIn("expected scenario failure", report["error"])
        self.assertFalse(report["testbeds"][0]["passed"])

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

        scenarios = {"hdmi_scan": scan, "activity": activity, "source_theft": source_theft}
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
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(main(scenarios, [*options, "--test", "all"]), 1)
            report = json.loads(output.read_text())
            self.assertEqual(called, ["scan", "activity", "source_theft"])
            self.assertFalse(report["passed"])
            self.assertTrue(report["tests"][0]["passed"])
            self.assertIn("physical standby command", report["tests"][1]["error"])
            self.assertTrue(report["tests"][2]["passed"])


if __name__ == "__main__":
    unittest.main()
