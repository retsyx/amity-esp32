"""Scenario coverage and resource ordering; no physical behavior qualification."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (Component, DUT, PlaybackComponent, PowerState,
                           assign_addresses)
from functional_tests import consortium


class Scenario:
    def __init__(self, *, failed_add=None, failed_theft=False):
        self.failed_add, self.failed_theft = failed_add, failed_theft
        self.events, self.members, self.activities = [], [], []
        self.components, self.duts, self.member_duts = (), (), ()
        self.current = None
        self.open = False

    @contextmanager
    def bed(self, *resources, members):
        self.duts = tuple(item for item in resources if isinstance(item, DUT))
        self.components = tuple(item for item in resources if isinstance(item, Component))
        self.member_duts = tuple(members)
        assign_addresses(self.components)
        self.open = True
        self.events.append(("bed.open",))
        try:
            yield self
        finally:
            if self.members or self.activities:
                raise AssertionError("Resources outlived their TestBed")
            self.events.append(("bed.close",))
            self.open = False

    def add(self, main, candidate):
        if not self.open or main is not self.duts[0] or candidate not in self.member_duts:
            raise AssertionError("Add must use the main and an owned headless candidate")
        self.events.append(("member.add", candidate.name))
        if candidate.name == self.failed_add:
            raise RuntimeError("candidate addition failed")
        fixture = self

        class Member:
            def __enter__(self):
                fixture.members.append(candidate)
                return self

            def assert_connected(self):
                expected = fixture.member_duts[:fixture.member_duts.index(candidate) + 1]
                if tuple(fixture.members) != expected:
                    raise AssertionError("A member's preserved-members baseline excludes later additions")
                fixture.events.append(("member.connected", candidate.name))
                return self

            def __exit__(self, *args):
                if fixture.activities or not fixture.open:
                    raise AssertionError("Activities must close before removing members with responders open")
                if fixture.members[-1] is not candidate:
                    raise AssertionError("Members must be removed in reverse add order")
                fixture.members.pop()
                fixture.events.append(("member.remove", candidate.name))

        return Member()

    def scan(self, main):
        if tuple(self.members) != self.member_duts:
            raise AssertionError("All members must be added before scanning")
        fixture = self

        class Scan:
            def assert_contains(self, *components):
                fixture.events.append(("scan.contains", tuple(components)))
                return self

            def assert_contains_only(self, *components):
                fixture.events.append(("scan.only", tuple(components)))
                return self

            def assert_on(self, component, endpoint):
                if component.dut is not endpoint.dut or component.direction != endpoint.direction:
                    raise AssertionError("The scan location must identify the actual owner and side")
                fixture.events.append(("scan.on", component.name, endpoint.dut.name, endpoint.direction))
                return self

        return Scan()

    def activity(self, main, name, *, display, audio, source):
        fixture = self
        triple = (display, audio, source)
        owner = source.dut.name

        class Activity:
            def __enter__(self):
                fixture.activities.append(self)
                fixture.events.append(("activity.create", owner))
                return self

            def start(self):
                if fixture.current is not None:
                    raise AssertionError("Each activity must complete its standby cycle")
                fixture.current = self
                for component in triple:
                    component._power = PowerState.ON
                source.active = True
                audio._active_input = source.address.physical
                fixture.events.append(("activity.start", owner))
                return SimpleNamespace(kind="start", owner=owner)

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The selected activity changed")
                fixture.events.append(("activity.active", owner))
                return self

            def standby(self):
                fixture.current = None
                for component in triple:
                    component._power = PowerState.STANDBY
                source.active = False
                fixture.events.append(("activity.standby", owner))
                return SimpleNamespace(kind="standby", owner=owner)

            def __exit__(self, *args):
                fixture.activities.remove(self)
                fixture.events.append(("activity.remove", owner))

        return Activity()

    def assert_power(self, component, power):
        if component.power != power:
            raise AssertionError(f"Unexpected {component.name} power")
        self.events.append(("power.state", component.dut.name, component.name, power))
        return component

    def wait_power(self, component, power, *, since, command=None, directed=False):
        if since.kind == "theft":
            if since.thief is not component or command != "standby" or not directed:
                raise AssertionError("Source theft requires fresh directed Standby evidence")
        elif since.owner != component.dut.name or directed or (since.kind == "standby" and command != "standby"):
            raise AssertionError("Activity power evidence belongs to the selected owner and operation")
        self.assert_power(component, power)
        self.events.append(("power.wait", since.kind, component.dut.name, component.name))
        return component

    def standby_after(self, component, announcement, *, within):
        if announcement.thief is not component or within != 0.250:
            raise AssertionError("Theft remediation must use this player's actual announcement and 250 ms bound")
        return self.wait_power(component, PowerState.STANDBY, since=announcement,
                               command="standby", directed=True)

    def route(self, avr, player, *, since):
        if avr.dut is not player.dut or since.kind != "start" or since.owner != avr.dut.name:
            raise AssertionError("Routing must use the active owner's start boundary")
        self.events.append(("route.wait", avr.dut.name))
        return avr

    def activate(self, player):
        if self.current is None:
            raise AssertionError("Theft must occur during an active activity")
        active_owner = next(event[1] for event in reversed(self.events) if event[0] == "activity.start")
        if active_owner == player.dut.name:
            raise AssertionError("An inactive owner must provide the thief")
        self.events.append(("theft", active_owner, player.dut.name))
        if self.failed_theft:
            raise TimeoutError("source theft failed")
        player._power, player.active = PowerState.STANDBY, False
        return SimpleNamespace(ok=True, kind="theft", thief=player)

    def run(self, test=consortium.test_consortium_three_duts):
        with ExitStack() as patches:
            patches.enter_context(patch.object(consortium, "TestBed", self.bed))
            patches.enter_context(patch.object(DUT, "add_amity", lambda main, candidate: self.add(main, candidate)))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda main: self.scan(main)))
            patches.enter_context(patch.object(DUT, "create_activity", lambda main, name, **kwargs: self.activity(main, name, **kwargs)))
            patches.enter_context(patch.object(Component, "assert_power", lambda component, power: self.assert_power(component, power)))
            patches.enter_context(patch.object(Component, "wait_for_power", lambda component, power, **kwargs: self.wait_power(component, power, **kwargs)))
            patches.enter_context(patch.object(Component, "wait_for_standby_after", lambda component, announcement, **kwargs: self.standby_after(component, announcement, **kwargs)))
            patches.enter_context(patch.object(Component, "wait_for_input", lambda avr, player, **kwargs: self.route(avr, player, **kwargs)))
            patches.enter_context(patch.object(Component, "checkpoint", lambda component: SimpleNamespace(kind="theft", thief=component)))
            patches.enter_context(patch.object(PlaybackComponent, "activate", lambda player: self.activate(player)))
            test()


class ConsortiumScenarioTests(unittest.TestCase):
    def assert_coverage(self, scenario, owners, *, main_name="amity-dut"):
        count = len(owners)
        self.assertEqual({dut.name for dut in scenario.duts}, owners)
        self.assertEqual(scenario.duts[0].name, main_name)
        self.assertEqual({dut.name for dut in scenario.member_duts}, owners - {main_name})
        self.assertEqual(len(scenario.components), count * 3)
        self.assertEqual(len({component.gpio for component in scenario.components}), count * 2)
        for kind in ("scan.contains", "scan.only"):
            self.assertEqual([row[1] for row in scenario.events if row[0] == kind], [scenario.components])
        expected_locations = {(component.name, component.dut.name, component.direction)
                              for component in scenario.components}
        locations = [row[1:] for row in scenario.events if row[0] == "scan.on"]
        self.assertEqual(len(locations), count * 3)
        self.assertEqual(set(locations), expected_locations)
        thefts = [row[1:] for row in scenario.events if row[0] == "theft"]
        self.assertEqual(len(thefts), count * (count - 1))
        self.assertEqual(set(thefts), {(active, thief) for active in owners for thief in owners if active != thief})
        for kind, expected in (("start", count * 3), ("standby", count * 3), ("theft", count * (count - 1))):
            self.assertEqual(sum(row[:2] == ("power.wait", kind) for row in scenario.events), expected)
        self.assertEqual(len([row for row in scenario.events if row[0] == "activity.active"]), count * count)
        self.assertEqual([row[1] for row in scenario.events if row[0] == "activity.start"],
                         [dut.name for dut in scenario.duts])
        self.assertEqual([row[1] for row in scenario.events if row[0] == "member.remove"],
                         [dut.name for dut in reversed(scenario.member_duts)])
        self.assertEqual(scenario.events[-1], ("bed.close",))

    def test_three_owners_have_exact_scan_locations_and_all_theft_directions(self):
        scenario = Scenario()
        scenario.run()
        self.assert_coverage(scenario, {"amity-dut", "amity-dut-2", "amity-dut-3"})

    def test_four_owners_have_exact_scan_locations_and_all_theft_directions(self):
        scenario = Scenario()
        scenario.run(consortium.test_consortium_four_duts)
        self.assert_coverage(scenario, {"amity-dut", "amity-dut-2", "amity-dut-3", "amity-dut-4"})
        self.assertEqual([row[1] for row in scenario.events if row[0] == "member.connected"],
                         ["amity-dut-2", "amity-dut-3", "amity-dut-4",
                          "amity-dut-4", "amity-dut-3", "amity-dut-2"])

    def test_fourth_board_as_main_preserves_all_scan_theft_and_cleanup_checks(self):
        scenario = Scenario()
        scenario.run(consortium.test_consortium_four_duts_main4)
        self.assert_coverage(scenario, {"amity-dut", "amity-dut-2", "amity-dut-3", "amity-dut-4"},
                             main_name="amity-dut-4")
        self.assertEqual([dut.name for dut in scenario.duts],
                         ["amity-dut-4", "amity-dut", "amity-dut-2", "amity-dut-3"])
        self.assertEqual([row[1] for row in scenario.events if row[0] == "member.connected"],
                         ["amity-dut", "amity-dut-2", "amity-dut-3",
                          "amity-dut-3", "amity-dut-2", "amity-dut"])

    def test_failed_third_add_removes_both_members_with_bed_open(self):
        scenario = Scenario(failed_add="amity-dut-4")
        with self.assertRaisesRegex(RuntimeError, "candidate addition failed"):
            scenario.run(consortium.test_consortium_four_duts)
        self.assertEqual([row[1] for row in scenario.events if row[0] == "member.remove"],
                         ["amity-dut-3", "amity-dut-2"])
        self.assertEqual(scenario.events[-1], ("bed.close",))

    def test_failed_second_add_still_removes_first_member_with_bed_open(self):
        scenario = Scenario(failed_add="amity-dut-3")
        with self.assertRaisesRegex(RuntimeError, "candidate addition failed"):
            scenario.run()
        self.assertEqual([row for row in scenario.events if row[0] == "member.remove"],
                         [("member.remove", "amity-dut-2")])
        self.assertEqual(scenario.events[-1], ("bed.close",))

    def test_failed_theft_closes_all_activities_before_removing_both_members(self):
        scenario = Scenario(failed_theft=True)
        with self.assertRaisesRegex(TimeoutError, "source theft failed"):
            scenario.run()
        cleanup = [row for row in scenario.events if row[0] in {"activity.remove", "member.remove", "bed.close"}]
        self.assertEqual([row[0] for row in cleanup],
                         ["activity.remove"] * 3 + ["member.remove"] * 2 + ["bed.close"])
        self.assertEqual([row[1] for row in cleanup if row[0] == "member.remove"],
                         ["amity-dut-3", "amity-dut-2"])

    def test_failed_four_owner_theft_closes_activities_before_removing_members(self):
        scenario = Scenario(failed_theft=True)
        with self.assertRaisesRegex(TimeoutError, "source theft failed"):
            scenario.run(consortium.test_consortium_four_duts)
        cleanup = [row for row in scenario.events if row[0] in {"activity.remove", "member.remove", "bed.close"}]
        self.assertEqual([row[0] for row in cleanup],
                         ["activity.remove"] * 4 + ["member.remove"] * 3 + ["bed.close"])
        self.assertEqual([row[1] for row in cleanup if row[0] == "member.remove"],
                         ["amity-dut-4", "amity-dut-3", "amity-dut-2"])


if __name__ == "__main__":
    unittest.main()
