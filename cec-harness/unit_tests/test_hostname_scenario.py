"""Hostname scenario ordering and required wire evidence; no hardware I/O."""

from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Component, DUT, Key, PowerState, TestBed, assign_addresses
from functional_tests import hostname as scenario


class HostnameScenario:
    def __init__(self, *, missing=None, stage="renamed", case=1, fail_rename=False, fail_verify=False,
                 fail_restore=False):
        self.missing, self.failed_stage = missing, stage
        self.failed_case = case
        self.fail_rename, self.fail_verify = fail_rename, fail_verify
        self.restore_failures = int(fail_restore)
        self.events, self.activities, self.created, self.members = [], [], [], []
        self.components, self.duts = (), ()
        self.hostnames = {}
        self.current, self.names = None, None
        self.case, self.stage = 1, "before"

    def bed(self, *resources, **kwargs):
        fixture = self
        self.duts = tuple(row for row in resources if isinstance(row, DUT))
        self.components = tuple(row for row in resources if isinstance(row, Component))
        self.hostnames = {dut.name: "amity" if dut.name == "amity-dut" else dut.name for dut in self.duts}

        class Session:
            def __init__(self, components):
                self.components = components
                self.records = []

            def __enter__(self):
                assign_addresses(self.components)
                fixture.events.append(("cec.open",))
                return self

            def assert_healthy(self):
                pass

            def __exit__(self, *args):
                for component in self.components:
                    component.address = None
                fixture.events.append(("cec.close",))

        self.testbed = TestBed(*resources, session_factory=Session, **kwargs)
        return self.testbed

    def add(self, main, candidate):
        fixture = self

        class Member:
            _cleanup_priority = 2

            def __init__(self):
                self.candidate, self.main = candidate, main
                self.device_id = candidate.identity.device_id
                fixture.members.append(self)
                main._testbed.add_cleanup(self.close)

            def assert_connected(self):
                fixture.events.append(("member.connected", candidate.name))

            def close(self):
                if self not in fixture.members:
                    return
                if fixture.activities:
                    raise AssertionError("Member cleanup overtook activities")
                fixture.require_canonical()
                fixture.members.remove(self)
                fixture.events.append(("member.close",))

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.close()
                return False

        return Member()

    def scan(self, main):
        fixture = self
        self.events.append(("scan", self.case, self.stage))

        class Scan:
            def assert_contains(self, *components):
                if components != fixture.components:
                    raise AssertionError("Scan lost test-owned equipment")

            assert_contains_only = assert_contains

            def assert_on(self, component, endpoint):
                if component.endpoint != endpoint:
                    raise AssertionError("Scan changed the physical attachment")

        return Scan()

    def activity(self, main, name, *, display, audio, source):
        fixture, components = self, (display, audio, source)

        class Activity:
            _cleanup_priority = 0
            retained_equipment = []

            def __init__(self):
                self.activity_id = f"{len(fixture.created) + 1:032x}"
                self.components, self._closed = components, False
                fixture.activities.append(self)
                fixture.created.append(self)
                main._testbed._activities.append(self)
                main._testbed.add_cleanup(self.close)

            def start(self):
                fixture.current = self
                for component in components:
                    component._power = PowerState.ON
                audio._active_input = source.address.physical
                frames = [] if fixture.fault("power") else list(components)
                action = SimpleNamespace(kind="start", frames=frames, route=not fixture.fault("route"))
                fixture.events.append(("start", fixture.case, fixture.stage, self, action))
                return action

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The saved activity is no longer current")

            def standby(self):
                fixture.current = None
                for component in components:
                    component._power = PowerState.STANDBY
                frames = [] if fixture.fault("standby") else list(components)
                return SimpleNamespace(kind="standby", frames=frames)

            def close(self):
                if self._closed:
                    return
                fixture.require_canonical()
                fixture.activities.remove(self)
                self._closed = True
                fixture.events.append(("activity.close", self))

        return Activity()

    def hostname_changes(self, main, *, members=(), activities=()):
        fixture = self
        if tuple(activities) != tuple(self.created) or tuple(members) != tuple(self.members):
            raise AssertionError("Hostname changes must retain the original members and activities")

        class Names:
            _cleanup_priority = -1

            def __init__(self):
                self.closed = False
                main._testbed.add_cleanup(self.close)

            def canonicalize(self):
                fixture.hostnames = {dut.name: dut.name for dut in fixture.duts}
                fixture.events.append(("canonicalize",))

            def rename(self, dut, hostname):
                if fixture.current or any(c.power != PowerState.STANDBY for c in fixture.components):
                    raise AssertionError("Hostname edit requires completed standby")
                fixture.require_canonical()
                fixture.hostnames[dut.name] = hostname
                fixture.stage = "renamed"
                fixture.events.append(("rename", dut.name, hostname))
                if fixture.fail_rename and fixture.case == fixture.failed_case:
                    raise AssertionError("Injected committed rename failure")

            def assert_hostname(self, dut, hostname):
                fixture.events.append(("verify", fixture.case, fixture.stage, dut.name, hostname))
                if (fixture.hostnames[dut.name] != hostname or fixture.fail_verify
                        and fixture.case == fixture.failed_case and fixture.stage == fixture.failed_stage):
                    raise AssertionError("Injected hostname verification failure")

            def reboot(self, *duts):
                if fixture.current or fixture.stage != "renamed":
                    raise AssertionError("Reboot must follow renamed activity standby")
                fixture.stage = "rebooted"
                fixture.events.append(("reboot", tuple(dut.name for dut in duts)))
                fixture.scan(main)  # Public helper may refresh the Activity boot proof.

            def restore(self):
                if fixture.restore_failures:
                    fixture.restore_failures -= 1
                    fixture.events.append(("restore.failed",))
                    raise RuntimeError("Injected canonical restoration failure")
                fixture.hostnames = {dut.name: dut.name for dut in fixture.duts}
                fixture.events.append(("restore", fixture.case))
                if fixture.stage != "before":
                    fixture.case += 1
                    fixture.stage = "before"

            def __enter__(self):
                return self

            def close(self):
                if not self.closed:
                    self.restore()
                    self.closed = True

            def __exit__(self, exc_type, exc, traceback):
                if exc is None:
                    self.close()
                return False

        self.names = Names()
        return self.names

    def require_canonical(self):
        if any(self.hostnames[dut.name] != dut.name for dut in self.duts):
            raise AssertionError("A test-owned hostname was not restored")

    def fault(self, kind):
        return self.missing == kind and self.stage == self.failed_stage and self.case == self.failed_case

    def power(self, component, state, *, since, command=None):
        if (component not in since.frames or component.power != state
                or command != ("standby" if since.kind == "standby" else None)):
            raise AssertionError("Missing fresh physical power command")
        self.events.append(("wire.power", self.case, self.stage, since.kind, component))

    def route(self, component, player, *, since):
        if not since.route or component.active_input != player.address.physical:
            raise AssertionError("Missing fresh physical routing command")
        self.events.append(("wire.route", self.case, self.stage, component))

    def key(self, main, key):
        fixture = self
        recipient = self.current.components[1 if key == Key.VOLUME_UP else 2]

        class Press:
            def assert_received_by(self, component):
                if component is not recipient or fixture.fault("key"):
                    raise AssertionError("Missing fresh physical key command")
                fixture.events.append(("wire.key", fixture.case, fixture.stage, key, component))

        return Press()

    def assert_power(self, component, state):
        if component.power != state:
            raise AssertionError("Unexpected component power")

    def run(self, test):
        with ExitStack() as patches:
            patches.enter_context(patch.object(scenario, "TestBed", self.bed))
            patches.enter_context(patch.object(scenario, "HostnameChanges", self.hostname_changes))
            patches.enter_context(patch.object(scenario, "event", side_effect=lambda name, **kw: self.events.append((name, kw.get("phase")))))
            patches.enter_context(patch.object(DUT, "authenticate", lambda dut: setattr(dut, "_token", "MOCK")))
            patches.enter_context(patch.object(DUT, "get_system", lambda dut: dict(octopus_id=dut.identity.device_id)))
            patches.enter_context(patch.object(DUT, "logout", lambda dut: setattr(dut, "_token", None)))
            patches.enter_context(patch.object(DUT, "close", lambda dut: None))
            patches.enter_context(patch.object(DUT, "add_amity", lambda main, candidate: self.add(main, candidate)))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda main: self.scan(main)))
            patches.enter_context(patch.object(DUT, "create_activity", lambda main, name, **kw: self.activity(main, name, **kw)))
            patches.enter_context(patch.object(DUT, "press_key", lambda main, key: self.key(main, key)))
            patches.enter_context(patch.object(DUT, "request", side_effect=AssertionError("Unexpected direct DUT request/reset")))
            patches.enter_context(patch.object(DUT, "press_fw_reset", side_effect=AssertionError("Unexpected GPIO reset")))
            patches.enter_context(patch.object(Component, "assert_power", lambda c, power: self.assert_power(c, power)))
            patches.enter_context(patch.object(Component, "wait_for_power", lambda c, power, **kw: self.power(c, power, **kw)))
            patches.enter_context(patch.object(Component, "wait_for_input", lambda c, player, **kw: self.route(c, player, **kw)))
            test()


class HostnameScenarioTests(unittest.TestCase):
    def assert_cleanup(self, fixture):
        fixture.require_canonical()
        self.assertFalse(fixture.activities)
        self.assertFalse(fixture.members)
        self.assertTrue(all(component.address is None for component in fixture.components))
        self.assertIn(("cec.close",), fixture.events)
        self.assertEqual(len([row for row in fixture.events if row[0] == "activity.close"]), len(fixture.created))

    def assert_retained(self, fixture):
        self.assertEqual(fixture.activities, fixture.created)
        self.assertEqual(len(fixture.members), len(fixture.duts) - 1)
        self.assertTrue(all(component.address is None for component in fixture.components))
        self.assertTrue(all(dut._token is None for dut in fixture.duts))
        self.assertIn(("cec.close",), fixture.events)
        self.assertFalse(any(row[0] in {"activity.close", "member.close"} for row in fixture.events))
        self.assertTrue(fixture.testbed.report["dut_state_retained"])

    def assert_fresh_commands(self, fixture, cases, owners):
        for case in range(1, cases + 1):
            for stage in ("before", "renamed", "rebooted"):
                with self.subTest(case=case, stage=stage):
                    for kind, count in (("wire.power", owners * 6), ("wire.route", owners), ("wire.key", owners * 2)):
                        self.assertEqual(len([row for row in fixture.events if row[:3] == (kind, case, stage)]), count)
                    starts = [row for row in fixture.events if row[:3] == ("start", case, stage)]
                    self.assertEqual([row[3] for row in starts], fixture.created)
                    self.assertEqual(len({id(row[4]) for row in starts}), owners)

    def test_local_hostname_is_verified_and_activity_retested_before_and_after_reboot(self):
        fixture = HostnameScenario()
        fixture.run(scenario.test_hostname_local)
        self.assertEqual([row for row in fixture.events if row[0] == "rename"], [("rename", "amity-dut", "amity-dut-t")])
        self.assertEqual([row for row in fixture.events if row[0] == "reboot"], [("reboot", ("amity-dut",))])
        self.assertEqual([row[2] for row in fixture.events if row[0] == "verify"], ["renamed", "rebooted"])
        self.assert_fresh_commands(fixture, 1, 1)
        self.assert_cleanup(fixture)

    def test_two_board_cases_cover_the_exact_requested_rename_and_reboot_order(self):
        fixture = HostnameScenario()
        fixture.run(scenario.test_hostname_consortium)
        self.assertEqual([row[1:] for row in fixture.events if row[0] == "rename"], [
            ("amity-dut", "amity-dut-t"), ("amity-dut", "amity-dut-t"),
            ("amity-dut-2", "amity-dut-2-t"), ("amity-dut-2", "amity-dut-2-t"),
            ("amity-dut", "amity-dut-t"), ("amity-dut-2", "amity-dut-2-t")])
        self.assertEqual([row[1] for row in fixture.events if row[0] == "reboot"], [
            ("amity-dut",), ("amity-dut-2",), ("amity-dut",), ("amity-dut-2",),
            ("amity-dut", "amity-dut-2"), ("amity-dut", "amity-dut-2")])
        self.assertEqual(len([row for row in fixture.events if row[0] == "verify"]), 12)
        self.assertEqual(len([row for row in fixture.events if row[0] == "scan"]), 7)
        self.assert_fresh_commands(fixture, 6, 2)
        self.assert_cleanup(fixture)

    def test_missing_fresh_commands_fail_even_with_correct_resulting_component_state(self):
        for test in (scenario.test_hostname_local, scenario.test_hostname_consortium):
            for stage in ("before", "renamed", "rebooted"):
                for missing in ("power", "route", "key", "standby"):
                    with self.subTest(test=test.__name__, stage=stage, missing=missing):
                        fixture = HostnameScenario(missing=missing, stage=stage)
                        with self.assertRaisesRegex(AssertionError, "Missing fresh physical"):
                            fixture.run(test)
                        self.assert_retained(fixture)
                        expected_name = "amity-dut" if stage == "before" else "amity-dut-t"
                        self.assertEqual(fixture.hostnames["amity-dut"], expected_name)
                        self.assertFalse(any(row[0] == "restore" for row in fixture.events))

    def test_committed_rename_or_verification_failure_retains_the_changed_hostname(self):
        for test in (scenario.test_hostname_local, scenario.test_hostname_consortium):
            for case in ((1,) if test is scenario.test_hostname_local else (1, 3)):
                for fault, stage in (("fail_rename", "renamed"), ("fail_verify", "renamed"),
                                     ("fail_verify", "rebooted")):
                    with self.subTest(test=test.__name__, case=case, fault=fault, stage=stage):
                        fixture = HostnameScenario(case=case, stage=stage, **{fault: True})
                        with self.assertRaisesRegex(AssertionError, "Injected"):
                            fixture.run(test)
                        self.assertTrue(any(row[0] == "rename" for row in fixture.events))
                        self.assert_retained(fixture)
                        renamed = "amity-dut-2" if case == 3 else "amity-dut"
                        self.assertEqual(fixture.hostnames[renamed], renamed + "-t")
                        self.assertEqual(len([row for row in fixture.events if row[0] == "restore"]), case - 1)

    def test_failed_activity_keeps_its_current_selection_and_never_attempts_restoration(self):
        fixture = HostnameScenario(missing="key", fail_restore=True)
        with self.assertRaisesRegex(AssertionError, "Missing fresh physical key"):
            fixture.run(scenario.test_hostname_consortium)
        self.assertFalse(any(row[0] in {"restore", "restore.failed"} for row in fixture.events))
        self.assertIs(fixture.current, fixture.created[0])
        self.assertTrue(all(component.power == PowerState.ON for component in fixture.current.components))
        self.assertEqual(fixture.hostnames["amity-dut"], "amity-dut-t")
        self.assert_retained(fixture)

    def test_case_restoration_failure_without_an_activity_error_still_fails(self):
        fixture = HostnameScenario(fail_restore=True)
        with self.assertRaisesRegex(RuntimeError, "Injected canonical restoration failure"):
            fixture.run(scenario.test_hostname_consortium)
        self.assertEqual(len([row for row in fixture.events if row[0] == "restore.failed"]), 1)
        self.assertFalse(any(row[0] == "restore" for row in fixture.events))
        self.assertEqual(fixture.hostnames["amity-dut"], "amity-dut-t")
        self.assert_retained(fixture)


if __name__ == "__main__":
    unittest.main()
