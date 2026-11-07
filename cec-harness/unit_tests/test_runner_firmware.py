"""Optional system firmware upgrades precede functional scenarios exactly once."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

from amity_harness.config import RunConfig, current_config
from amity_harness.runner import main


class FirmwareRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="amity-runner-firmware-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.firmware = self.directory / "amity-update.bin"
        self.firmware.write_bytes(b"update-file-fixture")
        self.options = ["--credentials", "/private/main.json", "--no-status-led"]

    def invoke(self, tests, options=(), *, upgrade=None):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return main(tests, [*self.options, *options], firmware_upgrade=upgrade)

    def test_upgrade_runs_once_before_every_selected_group(self):
        for selected, expected in (("activity", ["upgrade", "activity", "keys"]),
                                   ("all", ["upgrade", "activity", "keys", "scan"]),
                                   (None, ["upgrade", "activity", "keys"])):
            with self.subTest(selected=selected):
                calls = []

                def upgrade():
                    calls.append("upgrade")
                    config = current_config()
                    self.assertEqual(config.firmware, self.firmware)
                    self.assertEqual(config.firmware_timeout, 2700)
                    self.assertEqual(config.timeout, 180)
                    self.assertEqual(config.cec_timeout, 1)

                def activity():
                    calls.append("activity")

                def keys():
                    calls.append("keys")

                def scan():
                    calls.append("scan")

                options = ["--firmware", str(self.firmware)]
                if selected is not None:
                    options += ["--test", selected]
                self.assertEqual(self.invoke({"activity": (activity, keys), "scan": scan},
                                             options, upgrade=upgrade), 0)
                self.assertEqual(calls, expected)

    def test_standalone_upgrade_does_not_run_regular_scenarios(self):
        calls = []

        def upgrade():
            calls.append("upgrade")

        def ordinary():
            self.fail("A standalone upgrade ran an ordinary scenario")

        self.assertEqual(self.invoke({"ordinary": ordinary},
                                     ["--test", "firmware_upgrade", "--firmware", str(self.firmware)],
                                     upgrade=upgrade), 0)
        self.assertEqual(calls, ["upgrade"])

    def test_all_without_firmware_excludes_optional_upgrade(self):
        calls = []

        def upgrade():
            self.fail("--test all upgraded firmware without --firmware")

        def first():
            calls.append("first")
            self.assertIsNone(current_config().firmware)

        def second():
            calls.append("second")

        self.assertEqual(self.invoke({"first": first, "second": second}, ["--test", "all"],
                                     upgrade=upgrade), 0)
        self.assertEqual(calls, ["first", "second"])

    def test_upgrade_failure_stops_remaining_run_and_writes_failure(self):
        calls = []
        output = self.directory / "report.json"

        def upgrade():
            calls.append("upgrade")
            raise RuntimeError("Firmware did not become ready")

        def ordinary():
            self.fail("Scenario ran after the required upgrade failed")

        self.assertEqual(self.invoke({"first": ordinary, "second": ordinary},
                                     ["--test", "all", "--firmware", str(self.firmware),
                                      "--output", str(output)], upgrade=upgrade), 1)
        self.assertEqual(calls, ["upgrade"])
        report = json.loads(output.read_text())
        self.assertFalse(report["passed"])
        self.assertEqual(report["test"], "upgrade")
        self.assertEqual(report["error_type"], "RuntimeError")

    def test_later_scenario_failure_preserves_existing_continue_behavior(self):
        calls = []

        def upgrade():
            calls.append("upgrade")

        def failure():
            calls.append("failure")
            raise RuntimeError("Ordinary scenario failed")

        def following():
            calls.append("following")

        self.assertEqual(self.invoke({"ordinary": (failure, following)},
                                     ["--firmware", str(self.firmware)], upgrade=upgrade), 1)
        self.assertEqual(calls, ["upgrade", "failure", "following"])

    def test_firmware_timeout_is_independent_of_operation_and_cec_timeouts(self):
        observed = []

        def upgrade():
            config = current_config()
            observed.append((config.firmware_timeout, config.timeout, config.cec_timeout))

        self.assertEqual(self.invoke({"ordinary": lambda: None},
                                     ["--test", "firmware_upgrade", "--firmware", str(self.firmware),
                                      "--firmware-timeout", "3600", "--timeout", "23", "--cec-timeout", "7"],
                                     upgrade=upgrade), 0)
        self.assertEqual(observed, [(3600, 23, 7)])

    def test_missing_upgrade_image_or_callback_is_rejected_before_execution(self):
        def scenario():
            self.fail("Invalid CLI arguments executed a scenario")

        for options, upgrade in ((["--test", "firmware_upgrade"], scenario),
                                 (["--firmware", str(self.directory / "absent.bin")], scenario),
                                 (["--firmware", str(self.directory)], scenario),
                                 (["--firmware", str(self.firmware)], None)):
            with self.subTest(options=options), self.assertRaises(SystemExit) as raised:
                self.invoke({"ordinary": scenario}, options, upgrade=upgrade)
            self.assertEqual(raised.exception.code, 2)

    def test_cli_rejects_invalid_firmware_timeout(self):
        for value in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(value=value), self.assertRaises(SystemExit) as raised:
                self.invoke({"ordinary": lambda: self.fail("Invalid timeout executed")},
                            ["--firmware-timeout=" + value])
            self.assertEqual(raised.exception.code, 2)

    def test_config_normalizes_image_path_and_validates_firmware_timeout(self):
        config = RunConfig(firmware=str(self.firmware), firmware_timeout=3600)
        self.assertEqual(config.firmware, self.firmware)
        self.assertEqual((config.timeout, config.cec_timeout), (180, 1))
        for value in (True, False, 0, -1, math.inf, -math.inf, math.nan, "3600"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                RunConfig(firmware_timeout=value)


if __name__ == "__main__":
    unittest.main()
