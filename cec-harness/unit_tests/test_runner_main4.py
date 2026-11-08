"""Explicit main selection and reset/enrollment boundaries between soak phases."""

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import ANY, Mock, patch

from amity_harness.config import RunConfig, current_config, use_config
from amity_harness.runner import main, prepare_main, reset_main


class MainFourRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="amity-main4-")
        self.addCleanup(temporary.cleanup)
        self.firmware = Path(temporary.name) / "update.bin"
        self.firmware.write_bytes(b"image")
        self.calls = []

    def record(self, name):
        def scenario():
            config = current_config()
            self.calls.append((name, config.main_dut))
            self.assertEqual(config.credentials_for("amity-dut-4"), Path("/private/four.json"))
        scenario.__name__ = name
        return scenario

    def invoke(self, options, **overrides):
        soak1, soak4 = self.record("soak1"), self.record("soak4")
        registrations = dict(
            firmware_upgrade=self.record("upgrade1"),
            firmware_variants={"firmware_upgrade_main4": self.record("upgrade4")},
            main_duts={name: "amity-dut-4" for name in
                       ("consortium_four_duts_main4", "firmware_upgrade_main4")},
            opt_in_tests={"soak": (soak1, soak4),
                          "consortium_four_duts_main4": self.record("consortium4")},
            scenario_mains={soak4: "amity-dut-4"},
            prepare=self.record("prepare"), finish=self.record("finish"))
        registrations.update(overrides)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return main({"ordinary": self.record("ordinary")},
                        ["--credentials", "/private/one.json", "--dut-credentials",
                         "amity-dut-4=/private/four.json", "--no-status-led", *options],
                        **registrations)

    def test_main4_groups_prepare_and_reset_only_the_selected_main(self):
        self.assertEqual(self.invoke(["--test", "consortium_four_duts_main4"]), 0)
        self.assertEqual(self.calls, [(name, "amity-dut-4") for name in ("prepare", "consortium4", "finish")])

    def test_main4_firmware_prelude_matches_selected_role_and_runs_once(self):
        for group, expected in (("consortium_four_duts_main4", ("prepare", "upgrade4", "consortium4", "finish")),
                                ("firmware_upgrade_main4", ("prepare", "upgrade4", "finish"))):
            with self.subTest(group=group):
                self.calls.clear()
                self.assertEqual(self.invoke(["--test", group, "--firmware", str(self.firmware)]), 0)
                self.assertEqual(self.calls, [(name, "amity-dut-4") for name in expected])

    def test_all_excludes_alternate_main_variants_and_keeps_default_upgrade(self):
        self.assertEqual(self.invoke(["--test", "all", "--firmware", str(self.firmware)]), 0)
        self.assertEqual(self.calls, [(name, "amity-dut") for name in ("prepare", "upgrade1", "ordinary", "finish")])

    def test_both_soak_versions_require_explicit_selection(self):
        for options in ([], ["--test", "all"]):
            self.calls.clear()
            self.assertEqual(self.invoke(options), 0)
            self.assertEqual(self.calls, [(name, "amity-dut") for name in ("prepare", "ordinary", "finish")])
        for selection in (["--soak"], ["--test", "soak"]):
            self.calls.clear()
            self.assertEqual(self.invoke(selection), 0)
            self.assertEqual(self.calls, [(name, "amity-dut") for name in ("prepare", "soak1", "finish")]
                             + [(name, "amity-dut-4") for name in ("prepare", "soak4", "finish")])

    def test_shared_soak_upgrade_runs_once_before_both_versions(self):
        self.assertEqual(self.invoke(["--soak", "--firmware", str(self.firmware)]), 0)
        self.assertEqual(self.calls, [(name, "amity-dut") for name in ("prepare", "upgrade1", "soak1", "finish")]
                         + [(name, "amity-dut-4") for name in ("prepare", "soak4", "finish")])

    def test_soak_first_reverses_main_blocks_for_both_explicit_selectors(self):
        for selector in (["--soak"], ["--test", "soak"]):
            with self.subTest(selector=selector):
                self.calls.clear()
                self.assertEqual(self.invoke([*selector, "--soak-first", "amity-dut-4"]), 0)
                self.assertEqual(self.calls, [(name, "amity-dut-4") for name in ("prepare", "soak4", "finish")]
                                 + [(name, "amity-dut") for name in ("prepare", "soak1", "finish")])
        self.calls.clear()
        self.assertEqual(self.invoke(["--soak", "--soak-first", "amity-dut"]), 0)
        self.assertEqual(self.calls, [(name, "amity-dut") for name in ("prepare", "soak1", "finish")]
                         + [(name, "amity-dut-4") for name in ("prepare", "soak4", "finish")])

    def test_soak_first_preserves_all_scenarios_inside_each_main_block(self):
        one_a, one_b = self.record("one_a"), self.record("one_b")
        four_a, four_b = self.record("four_a"), self.record("four_b")
        self.assertEqual(self.invoke(["--soak", "--soak-first", "amity-dut-4"],
            opt_in_tests={"soak": (one_a, one_b, four_a, four_b)},
            scenario_mains={four_a: "amity-dut-4", four_b: "amity-dut-4"},
            main_duts={"firmware_upgrade_main4": "amity-dut-4"}), 0)
        self.assertEqual(self.calls, [(name, "amity-dut-4") for name in ("prepare", "four_a", "four_b", "finish")]
                         + [(name, "amity-dut") for name in ("prepare", "one_a", "one_b", "finish")])

    def test_soak_first_upgrade_matches_first_main_and_still_runs_once(self):
        self.assertEqual(self.invoke(["--soak", "--soak-first", "amity-dut-4", "--firmware", str(self.firmware)]), 0)
        self.assertEqual(self.calls, [(name, "amity-dut-4") for name in ("prepare", "upgrade4", "soak4", "finish")]
                         + [(name, "amity-dut") for name in ("prepare", "soak1", "finish")])

    def test_soak_first_requires_soak_and_a_registered_main_before_execution(self):
        for options in (["--soak-first", "amity-dut-4"],
                        ["--test", "all", "--soak-first", "amity-dut-4"],
                        ["--test", "consortium_four_duts_main4", "--soak-first", "amity-dut-4"],
                        ["--soak", "--soak-first", "amity-dut-2"]):
            with self.subTest(options=options), self.assertRaises(SystemExit) as rejected:
                self.invoke(options)
            self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_soak_first_missing_role_upgrade_is_rejected_before_enrollment(self):
        with self.assertRaises(SystemExit) as rejected:
            self.invoke(["--soak", "--soak-first", "amity-dut-4", "--firmware", str(self.firmware)],
                        firmware_variants={}, main_duts={"consortium_four_duts_main4": "amity-dut-4"})
        self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_soak_selector_rejects_independent_main4_selection_and_conflicting_flags(self):
        for selection in (["--test", "soak_main4"], ["--soak", "--test", "soak"]):
            with self.subTest(selection=selection), self.assertRaises(SystemExit) as rejected:
                self.invoke(selection)
            self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_main4_firmware_requires_image_before_enrollment(self):
        with self.assertRaises(SystemExit) as rejected:
            self.invoke(["--test", "firmware_upgrade_main4"])
        self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_missing_role_matched_firmware_is_rejected_before_enrollment(self):
        with self.assertRaises(SystemExit) as rejected:
            self.invoke(["--test", "consortium_four_duts_main4", "--firmware", str(self.firmware)],
                        firmware_variants={}, main_duts={"consortium_four_duts_main4": "amity-dut-4"})
        self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_failed_required_main4_upgrade_preserves_main_and_skips_scenarios(self):
        def failure():
            self.assertEqual(current_config().main_dut, "amity-dut-4")
            self.calls.append(("failed_upgrade", current_config().main_dut))
            raise RuntimeError("OTA failed")
        self.assertEqual(self.invoke(
            ["--test", "consortium_four_duts_main4", "--firmware", str(self.firmware)],
            firmware_variants={"firmware_upgrade_main4": failure}), 1)
        self.assertEqual(self.calls, [(name, "amity-dut-4") for name in ("prepare", "failed_upgrade")])

    def test_inconsistent_role_registrations_are_rejected_before_execution(self):
        invalid = [dict(main_duts={"missing": "amity-dut-4"}),
                   dict(main_duts={"ordinary": "amity-dut-4"}),
                   dict(main_duts={"consortium_four_duts_main4": ""}),
                   dict(main_duts={}),  # Both upgrade callbacks would target the default main.
                   dict(firmware_variants={"ordinary": lambda: None}),
                   dict(firmware_variants={"firmware_upgrade_main4": None})]
        for registration in invalid:
            with self.subTest(registration=registration), self.assertRaises(ValueError):
                self.invoke(["--test", "all"], **registration)
        self.assertEqual(self.calls, [])

    def test_failed_soak_or_transition_callback_blocks_next_main(self):
        for failure_step in ("soak1", "finish1", "prepare4", "soak4", "finish4"):
            with self.subTest(failure_step=failure_step):
                self.calls.clear()

                def phase(name):
                    def run():
                        owner = current_config().main_dut
                        step = name + ("4" if owner == "amity-dut-4" else "1")
                        self.calls.append((step, owner))
                        if step == failure_step:
                            raise RuntimeError("injected lifecycle failure")
                    run.__name__ = name
                    return run

                soak1, soak4 = phase("soak"), phase("soak")
                self.assertEqual(self.invoke(["--soak"],
                    opt_in_tests={"soak": (soak1, soak4)}, scenario_mains={soak4: "amity-dut-4"},
                    main_duts={"firmware_upgrade_main4": "amity-dut-4"},
                    prepare=phase("prepare"), finish=phase("finish")), 1)
                expected = [(name, "amity-dut") for name in ("prepare1", "soak1", "finish1")]
                expected += [(name, "amity-dut-4") for name in ("prepare4", "soak4", "finish4")]
                self.assertEqual(self.calls, expected[:[name for name, _ in expected].index(failure_step) + 1])

    def test_reverse_soak_failure_preserves_failed_main_and_stops_lifecycle(self):
        expected = [(name, "amity-dut-4") for name in ("prepare4", "soak4", "finish4")]
        expected += [(name, "amity-dut") for name in ("prepare1", "soak1", "finish1")]
        for failure_step, _ in expected:
            with self.subTest(failure_step=failure_step):
                self.calls.clear()

                def phase(name):
                    def run():
                        owner = current_config().main_dut
                        step = name + ("4" if owner == "amity-dut-4" else "1")
                        self.calls.append((step, owner))
                        if step == failure_step:
                            raise RuntimeError("injected lifecycle failure")
                    run.__name__ = name
                    return run

                soak1, soak4 = phase("soak"), phase("soak")
                self.assertEqual(self.invoke(["--soak", "--soak-first", "amity-dut-4"],
                    opt_in_tests={"soak": (soak1, soak4)}, scenario_mains={soak4: "amity-dut-4"},
                    main_duts={"firmware_upgrade_main4": "amity-dut-4"},
                    prepare=phase("prepare"), finish=phase("finish")), 1)
                self.assertEqual(self.calls, expected[:[name for name, _ in expected].index(failure_step) + 1])

    def test_failed_reverse_soak_upgrade_keeps_first_main_without_reset(self):
        def failure():
            self.calls.append(("failed_upgrade", current_config().main_dut))
            raise RuntimeError("OTA failed")
        self.assertEqual(self.invoke(["--soak", "--soak-first", "amity-dut-4", "--firmware", str(self.firmware)],
            firmware_variants={"firmware_upgrade_main4": failure}), 1)
        self.assertEqual(self.calls, [(name, "amity-dut-4") for name in ("prepare", "failed_upgrade")])

    def test_main_transition_preserves_urls_and_uses_each_boards_private_credentials(self):
        seen = []

        def inspect():
            config = current_config()
            owner = config.main_dut
            commissioner = config.enrollment_factory()
            seen.append((owner, config.credentials_for(owner), commissioner.credentials))
            if owner == "amity-dut":
                config.admin_origins["amity-dut"] = "https://first.local"
            else:
                self.assertEqual(config.admin_origins["amity-dut"], "https://first.local")
                self.assertEqual(config.url_for("amity-dut-4"), "https://four.local")

        soak1, soak4 = inspect, lambda: inspect()
        self.assertEqual(self.invoke(["--soak", "--enrollment-credentials", "/private/enrollment1.json",
            "--dut-enrollment-credentials", "amity-dut-4=/private/enrollment4.json",
            "--dut-url", "amity-dut-4=https://four.local"],
            opt_in_tests={"soak": (soak1, soak4)}, scenario_mains={soak4: "amity-dut-4"},
            main_duts={"firmware_upgrade_main4": "amity-dut-4"}, prepare=inspect, finish=inspect), 0)
        self.assertEqual(seen, [("amity-dut", Path("/private/one.json"), Path("/private/enrollment1.json"))] * 3
                         + [("amity-dut-4", Path("/private/four.json"), Path("/private/enrollment4.json"))] * 3)

    def test_reverse_transition_preserves_origins_credentials_and_enrollment_per_owner(self):
        seen = []

        def inspect():
            config = current_config()
            owner = config.main_dut
            commissioner = config.enrollment_factory()
            seen.append((owner, config.credentials_for(owner), commissioner.credentials))
            if owner == "amity-dut-4":
                config.admin_origins[owner] = "https://four.local"
            else:
                self.assertEqual(config.admin_origins["amity-dut-4"], "https://four.local")
                self.assertEqual(config.url_for(owner), "https://one.local")

        soak1, soak4 = inspect, lambda: inspect()
        self.assertEqual(self.invoke(["--test", "soak", "--soak-first", "amity-dut-4",
            "--enrollment-credentials", "/private/enrollment1.json",
            "--dut-enrollment-credentials", "amity-dut-4=/private/enrollment4.json",
            "--dut-url", "amity-dut=https://one.local"],
            opt_in_tests={"soak": (soak1, soak4)}, scenario_mains={soak4: "amity-dut-4"},
            main_duts={"firmware_upgrade_main4": "amity-dut-4"}, prepare=inspect, finish=inspect), 0)
        self.assertEqual(seen, [("amity-dut-4", Path("/private/four.json"), Path("/private/enrollment4.json"))] * 3
                         + [("amity-dut", Path("/private/one.json"), Path("/private/enrollment1.json"))] * 3)

    def test_role_changes_require_both_preparation_and_reset_before_any_execution(self):
        for callbacks in (dict(prepare=None, finish=None), dict(finish=None)):
            with self.subTest(callbacks=callbacks), self.assertRaises(SystemExit) as rejected:
                self.invoke(["--soak"], **callbacks)
            self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_enrollment_overrides_require_unique_dut_path_entries(self):
        for values in (("amity-dut-4=",), ("=path.json",), ("amity-dut-4=a", "amity-dut-4=b")):
            options = [argument for value in values for argument in ("--dut-enrollment-credentials", value)]
            with self.subTest(values=values), self.assertRaises(SystemExit) as rejected:
                self.invoke(["--soak", *options])
            self.assertEqual(rejected.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_prepare_and_reset_construct_the_configured_main_and_verify_its_identity(self):
        commissioner = Mock()
        with use_config(RunConfig(main_dut="amity-dut-4", enrollment_factory=lambda: commissioner)), \
                patch("amity_harness.dut.DUT") as dut_type, \
                patch("amity_harness.enrollment.ensure_enrolled") as ensure, \
                patch("amity_harness.enrollment._probe_ready", side_effect=ConnectionRefusedError()):
            dut = dut_type.return_value
            dut.name = "amity-dut-4"
            dut.get_system.return_value = {"octopus_id": dut.identity.device_id}
            dut.request.return_value = {"rebooting": True}
            prepare_main()
            ensure.assert_called_once_with(dut, commissioner, timeout=240)
            reset_main()
            self.assertEqual([call.args for call in dut_type.call_args_list], [("amity-dut-4",), ("amity-dut-4",)])
            dut.request.assert_called_once_with("/api/factory-reset", {"confirm": True})
            commissioner.verify_factory.assert_called_once_with(dut, timeout=ANY)

    def test_main_dut_config_requires_a_name(self):
        self.assertEqual(RunConfig().main_dut, "amity-dut")
        for value in (None, "", True, 4):
            with self.subTest(value=value), self.assertRaises(ValueError):
                RunConfig(main_dut=value)


if __name__ == "__main__":
    unittest.main()
