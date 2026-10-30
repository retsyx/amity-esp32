"""Generic bed/config lifetime checks without DUT, GPIO, radio or SSH access."""

from pathlib import Path
from types import SimpleNamespace
import unittest

from amity_harness.components import TvComponent
from amity_harness.config import RunConfig, current_config, track_testbeds, use_config
from amity_harness.dut import DUT
from amity_harness.testbed import TestBed, TestBedCleanupError
from amity_harness.topology import CecAddress
from unit_tests.harness_pins import cec_gpio


class UnitWiring:
    def cec_gpio(self, name, tentacle, direction):
        board = "amity-dut" if name == "first-board" else "amity-dut-2"
        return cec_gpio(board, tentacle, direction)


class UnitDUT(DUT):
    def __init__(self, name, log, *, identity=None, auth_error=False,
                 wrong_identity=False, logout_error=False):
        self.name = name
        self.identity = SimpleNamespace(device_id=identity or name)
        self.wiring = UnitWiring()
        self.log = log
        self.auth_error = auth_error
        self.wrong_identity = wrong_identity
        self.logout_error = logout_error
        self._testbed = self._token = self.last_scan = None

    def authenticate(self):
        self.log.append((self.name, "authenticate"))
        self._token = "PRIVATE_TOKEN"
        if self.auth_error:
            raise RuntimeError("synthetic auth failure")

    def get_system(self):
        self.log.append((self.name, "identity"))
        return {"octopus_id": "wrong" if self.wrong_identity else self.identity.device_id}

    def logout(self):
        self.log.append((self.name, "logout"))
        self._token = None
        if self.logout_error:
            raise ValueError("PRIVATE_LOGOUT_ERROR")


class UnitSession:
    def __init__(self, components, log, *, enter_error=False, close_error=False, unhealthy=False):
        self.components = list(components)
        self.log = log
        self.records = []
        self.enter_error = enter_error
        self.close_error = close_error
        self.unhealthy = unhealthy

    def __enter__(self):
        self.log.append(("session", "enter"))
        if self.enter_error:
            raise OSError("synthetic CEC acquisition failure")
        return self

    def assert_healthy(self):
        if self.unhealthy:
            raise ArithmeticError("synthetic unhealthy worker")

    def close(self):
        self.log.append(("session", "close"))
        if self.close_error:
            raise OSError("PRIVATE_SESSION_ERROR")

    def __exit__(self, exc_type, exc, traceback):
        self.close()


class RunConfigTests(unittest.TestCase):
    def test_per_dut_overrides_fallbacks_and_frozen_copies(self):
        paths = {"first-board": Path("/tmp/first-private.json")}
        urls = {"first-board": "https://first.example"}
        config = RunConfig(credentials=paths, default_credentials=Path("/tmp/default-private.json"),
                           urls=urls, default_url="https://default.example")
        paths.clear()
        urls.clear()
        self.assertEqual(config.credentials_for("first-board"), Path("/tmp/first-private.json"))
        self.assertEqual(config.credentials_for("other"), Path("/tmp/default-private.json"))
        self.assertEqual(config.url_for("first-board"), "https://first.example")
        self.assertEqual(config.url_for("other"), "https://default.example")
        with self.assertRaises(TypeError):
            config.urls["other"] = "https://mutated.example"

    def test_nested_config_and_collectors_reset_after_exceptions(self):
        original = current_config()
        log = []
        outer, inner = RunConfig(timeout=13), RunConfig(timeout=17)
        with use_config(outer), track_testbeds() as outer_beds:
            self.assertIs(current_config(), outer)
            one = TestBed(UnitDUT("first-board", log))
            with self.assertRaisesRegex(RuntimeError, "leave inner"):
                with use_config(inner), track_testbeds() as inner_beds:
                    self.assertIs(current_config(), inner)
                    two = TestBed(UnitDUT("second-board", log))
                    self.assertEqual(inner_beds, [two])
                    raise RuntimeError("leave inner")
            self.assertIs(current_config(), outer)
            self.assertEqual(outer_beds, [one])
        self.assertIs(current_config(), original)
        TestBed(UnitDUT("uncollected", log))
        self.assertEqual(outer_beds, [one])

    def test_invalid_timeout_and_conflicting_tls_fail_without_io(self):
        for setting in ("timeout", "cec_timeout"):
            for timeout in (0, -1, True, float("nan"), float("inf")):
                with self.subTest(setting=setting, timeout=timeout), self.assertRaises(ValueError):
                    RunConfig(**{setting: timeout})
        with self.assertRaises(ValueError):
            RunConfig(cafile=Path("/tmp/ca.pem"), insecure=True)
        with self.assertRaises(ValueError):
            RunConfig(gpio_factory=object())


class TestBedLifetimeTests(unittest.TestCase):
    def setUp(self):
        self.log = []

    def factory(self, **options):
        def create(components):
            self.log.append(("session", "construct"))
            return UnitSession(components, self.log, **options)
        return create

    def test_multi_dut_inference_authentication_precedes_session_and_cleanup(self):
        first, second = UnitDUT("first-board", self.log), UnitDUT("second-board", self.log)
        front = TvComponent(first, "First TV", address=CecAddress(0, 0))
        other = TvComponent(second, "Second TV", address=CecAddress(0, 0))
        with use_config(RunConfig(session_factory=self.factory())):
            bed = TestBed(front, second, other)
        with bed:
            self.assertEqual(bed.duts, (first, second))
            self.assertEqual(bed.components, (front, other))
            self.assertIs(first._testbed, bed)
            self.assertIs(second._testbed, bed)
        self.assertEqual(self.log, [
            ("first-board", "authenticate"), ("first-board", "identity"),
            ("second-board", "authenticate"), ("second-board", "identity"),
            ("session", "construct"), ("session", "enter"), ("session", "close"),
            ("second-board", "logout"), ("first-board", "logout")])
        self.assertIsNone(first._testbed)
        self.assertIsNone(second._testbed)
        self.assertTrue(bed.report["passed"])
        with self.assertRaises(RuntimeError):
            bed.__enter__()

    def test_identity_failure_cleans_login_without_constructing_cec_session(self):
        dut = UnitDUT("first-board", self.log, wrong_identity=True)
        with track_testbeds() as beds:
            bed = TestBed(dut, session_factory=self.factory())
            with self.assertRaisesRegex(AssertionError, "identity"):
                with bed:
                    self.fail("Identity mismatch must prevent entering the test")
        self.assertEqual(beds, [bed])
        self.assertNotIn(("session", "construct"), self.log)
        self.assertEqual(self.log[-1], (dut.name, "logout"))
        self.assertFalse(bed.report["passed"])
        self.assertIsNone(dut._token)
        self.assertIsNone(dut._testbed)

    def test_control_only_candidate_is_owned_and_reported_without_any_http(self):
        main = UnitDUT("first-board", self.log)
        candidate = UnitDUT("second-board", self.log, auth_error=True, wrong_identity=True)
        bed = TestBed(main, candidate, control_only=(candidate,), session_factory=self.factory())
        with bed:
            self.assertEqual(bed.control_only, (candidate,))
            self.assertIs(candidate._testbed, bed)
            self.assertIsNone(candidate._token)
            alias = UnitDUT("alias-board", self.log, identity=candidate.identity.device_id)
            with self.assertRaisesRegex(RuntimeError, "active"):
                TestBed(alias, control_only=(alias,), session_factory=self.factory()).__enter__()
            self.assertIs(candidate._testbed, bed)
        self.assertFalse(any(name == candidate.name for name, _ in self.log))
        self.assertIsNone(candidate._testbed)
        self.assertEqual(bed.report["duts"], [
            {"name": main.name, "device_id": main.identity.device_id, "control_only": False},
            {"name": candidate.name, "device_id": candidate.identity.device_id, "control_only": True}])

    def test_headless_member_hosts_components_without_http_and_loses_proof_at_exit(self):
        main = UnitDUT("first-board", self.log)
        member = UnitDUT("second-board", self.log, auth_error=True, wrong_identity=True)
        tv = TvComponent(member, "Member TV", address=CecAddress(0, 0))
        bed = TestBed(main, member, tv, members=(member,), session_factory=self.factory())
        with bed:
            self.assertEqual(bed.members, (member,))
            self.assertIsNone(member._token)
            bed._member_proofs[member.identity.device_id] = object()
            self.assertEqual(bed.session.components, [tv])
        self.assertFalse(any(name == member.name for name, _ in self.log))
        self.assertEqual(bed._member_proofs, {})
        self.assertIsNone(member._testbed)
        self.assertTrue(bed.report["passed"])
        self.assertTrue(bed.report["duts"][1]["member"])

    def test_invalid_headless_member_resources_fail_before_io(self):
        main = UnitDUT("first-board", self.log)
        member = UnitDUT("second-board", self.log)
        for resources, members, controls in (((main,), (member,), ()),
                                             ((main, member), (member, member), ()),
                                             ((main, member), (member,), (member,)),
                                             ((member,), (member,), ())):
            with self.subTest(members=members), self.assertRaises(ValueError):
                TestBed(*resources, members=members, control_only=controls, session_factory=self.factory())
        member._token = "preexisting-private-session"
        with self.assertRaisesRegex(ValueError, "HTTP"):
            TestBed(main, member, members=(member,), session_factory=self.factory())
        self.assertFalse(self.log)

    def test_invalid_control_only_resources_fail_before_any_io(self):
        main, candidate = UnitDUT("first-board", self.log), UnitDUT("second-board", self.log)
        for controls in ((candidate,), ("second-board",), (main, main)):
            with self.subTest(controls=controls), self.assertRaises(ValueError):
                TestBed(main, control_only=controls, session_factory=self.factory())
        component = TvComponent(candidate, "Candidate TV", address=CecAddress(0, 0))
        with self.assertRaisesRegex(ValueError, "CEC components"):
            TestBed(main, component, control_only=(candidate,), session_factory=self.factory())
        candidate._token = "PREEXISTING_TOKEN"
        with self.assertRaisesRegex(ValueError, "authenticated"):
            TestBed(main, candidate, control_only=(candidate,), session_factory=self.factory())
        self.assertEqual(candidate._token, "PREEXISTING_TOKEN")
        self.assertFalse(self.log)

    def test_partial_login_failure_logs_out_all_owned_logins(self):
        first = UnitDUT("first-board", self.log)
        second = UnitDUT("second-board", self.log, auth_error=True)
        bed = TestBed(first, second, session_factory=self.factory())
        with self.assertRaisesRegex(RuntimeError, "auth failure"):
            bed.__enter__()
        self.assertEqual(self.log[-2:], [(second.name, "logout"), (first.name, "logout")])
        self.assertNotIn(("session", "construct"), self.log)
        self.assertIsNone(first._testbed)
        self.assertIsNone(second._testbed)

    def test_failed_session_entry_is_closed_before_logout(self):
        dut = UnitDUT("first-board", self.log)
        bed = TestBed(dut, session_factory=self.factory(enter_error=True))
        with self.assertRaises(OSError):
            bed.__enter__()
        self.assertEqual(self.log[-2:], [("session", "close"), (dut.name, "logout")])
        self.assertFalse(bed.report["passed"])

    def test_foreign_active_or_alias_dut_is_rejected_without_foreign_cleanup(self):
        dut = UnitDUT("first-board", self.log)
        with TestBed(dut, session_factory=self.factory()) as owner:
            foreign = TestBed(dut, session_factory=self.factory())
            before = list(self.log)
            with self.assertRaisesRegex(RuntimeError, "active"):
                foreign.__enter__()
            self.assertEqual(self.log, before)
            self.assertIs(dut._testbed, owner)
            self.assertEqual(foreign.report["scans"], [])
            alias = UnitDUT("alias-board", self.log, identity=dut.identity.device_id)
            with self.assertRaisesRegex(RuntimeError, "active"):
                TestBed(alias, session_factory=self.factory()).__enter__()
            self.assertIsNone(alias._testbed)
            self.assertIsNotNone(dut._token)

    def test_duplicate_resources_and_physical_identities_are_rejected(self):
        dut = UnitDUT("first-board", self.log)
        with self.assertRaises(ValueError):
            TestBed(dut, dut)
        alias = UnitDUT("alias-board", self.log, identity=dut.identity.device_id)
        with self.assertRaises(ValueError):
            TestBed(dut, alias)
        self.assertFalse(self.log)

    def test_all_cleanup_failures_are_reported_after_every_resource_is_attempted(self):
        first = UnitDUT("first-board", self.log, logout_error=True)
        second = UnitDUT("second-board", self.log, logout_error=True)
        bed = TestBed(first, second, session_factory=self.factory(close_error=True))
        with self.assertRaises(TestBedCleanupError) as raised:
            with bed:
                pass
        self.assertEqual(len(raised.exception.errors), 3)
        self.assertEqual(self.log[-3:], [("session", "close"), (second.name, "logout"), (first.name, "logout")])
        self.assertEqual(bed.report["cleanup_errors"], ["OSError", "ValueError", "ValueError"])
        self.assertFalse(bed.report["passed"])
        self.assertIsNone(first._testbed)
        self.assertIsNone(second._testbed)

    def test_owned_ui_cleanup_runs_lifo_before_cec_and_logout_on_failure(self):
        dut = UnitDUT("first-board", self.log)
        bed = TestBed(dut, session_factory=self.factory())

        def cleanup(label):
            self.assertIsNotNone(dut._token)
            self.assertIs(dut._testbed, bed)
            self.assertFalse(bed._closed)
            self.log.append((label, "cleanup"))

        with self.assertRaisesRegex(AssertionError, "scenario failure"):
            with bed:
                bed.add_cleanup(lambda: cleanup("first"))
                bed.add_cleanup(lambda: cleanup("second"))
                raise AssertionError("scenario failure")
        self.assertEqual(self.log[-4:], [("second", "cleanup"), ("first", "cleanup"),
                                        ("session", "close"), (dut.name, "logout")])
        self.assertEqual(bed.report["cleanup_errors"], [])

    def test_ui_cleanup_failure_does_not_skip_other_cleanup_or_cec_release(self):
        dut = UnitDUT("first-board", self.log)
        bed = TestBed(dut, session_factory=self.factory())

        def broken():
            self.log.append(("broken", "cleanup"))
            raise LookupError("synthetic UI cleanup failure")

        with self.assertRaises(TestBedCleanupError):
            with bed:
                bed.add_cleanup(lambda: self.log.append(("other", "cleanup")))
                bed.add_cleanup(broken)
        self.assertEqual(self.log[-4:], [("broken", "cleanup"), ("other", "cleanup"),
                                        ("session", "close"), (dut.name, "logout")])
        self.assertEqual(bed.report["cleanup_errors"], ["LookupError"])
        self.assertFalse(bed.report["passed"])
        self.assertIsNone(dut._testbed)

    def test_cleanup_registration_requires_active_test_bed(self):
        dut = UnitDUT("first-board", self.log)
        bed = TestBed(dut, session_factory=self.factory())
        with self.assertRaises(RuntimeError):
            bed.add_cleanup(lambda: None)
        with bed:
            with self.assertRaises(TypeError):
                bed.add_cleanup(None)
        with self.assertRaises(RuntimeError):
            bed.add_cleanup(lambda: None)

    def test_body_exception_propagates_after_cleanup_and_worker_failure_cannot_pass(self):
        dut = UnitDUT("first-board", self.log)
        bed = TestBed(dut, session_factory=self.factory())
        with self.assertRaisesRegex(LookupError, "assertion"):
            with bed:
                raise LookupError("assertion")
        self.assertFalse(bed.report["passed"])
        self.assertEqual(self.log[-1], (dut.name, "logout"))
        dut = UnitDUT("first-board", self.log)
        unhealthy = TestBed(dut, session_factory=self.factory(unhealthy=True))
        with self.assertRaises(ArithmeticError):
            unhealthy.__enter__()
        self.assertFalse(unhealthy.report["passed"])
        self.assertIsNone(dut._testbed)

    def test_report_keeps_failed_scan_evidence_without_credentials_or_mutable_aliases(self):
        dut = UnitDUT("first-board", self.log)
        bed = TestBed(dut, session_factory=self.factory())
        with self.assertRaises(TimeoutError):
            with bed:
                dut.last_scan = SimpleNamespace(
                    started=42, operation={"stage": "complete", "password": "PRIVATE_PASSWORD"},
                    devices=[{"title": "Unit TV", "bus": "front", "password": "PRIVATE_PASSWORD"}],
                    status={"discovering": True, "expected_boot_id": "boot", "token": "PRIVATE_TOKEN", "devices": []},
                    records=({"component": "Unit TV", "frame": "40:83", "transport": "wire", "Authorization": "PRIVATE_TOKEN"},),
                    initiators={cec_gpio(direction="front"): 4})
                bed.session.records = list(dut.last_scan.records)
                raise TimeoutError("synthetic scan timeout")
        report = bed.report
        self.assertEqual(report["scans"][0]["started_monotonic_ns"], 42)
        self.assertTrue(report["scans"][0]["last_state"]["discovering"])
        self.assertEqual(report["cec"][0]["frame"], "40:83")
        self.assertNotIn("PRIVATE_", repr(report))
        report["scans"][0]["devices"][0]["title"] = "tampered"
        self.assertEqual(bed.report["scans"][0]["devices"][0]["title"], "Unit TV")

    def test_activity_reports_exclude_secrets_and_prior_bed_operations(self):
        dut = UnitDUT("first-board", self.log)
        dut.activity_operations = [{"phase": "prior", "name": "another run"}]
        bed = TestBed(dut, session_factory=self.factory())
        with bed:
            dut.activity_operations.append({
                "phase": "start", "activity_id": "a" * 32, "password": "PRIVATE_PASSWORD",
                "operation": {"stage": "complete", "token": "PRIVATE_TOKEN"},
                "state": {"current_activity": 0, "Authorization": "PRIVATE_TOKEN"}})
        report = bed.report
        self.assertEqual(len(report["activities"]), 1)
        self.assertEqual(report["activities"][0]["state"], {"current_activity": 0})
        self.assertNotIn("PRIVATE_", repr(report))
        report["activities"][0]["operation"]["stage"] = "tampered"
        self.assertEqual(bed.report["activities"][0]["operation"]["stage"], "complete")


if __name__ == "__main__":
    unittest.main()
