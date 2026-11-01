"""Host checks for dual-display coverage and failure propagation, not wire qualification."""

from contextlib import ExitStack
from importlib import import_module
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, Component, DUT, PlaybackComponent, PowerState, TvComponent
from unit_tests.test_consortium_scenarios import Scenario

dual_display = import_module("functional_tests.dual-display")


class DualDisplayScenario(Scenario):
    """Model baseline activity differences; evidence is separate from model state."""

    def __init__(self, *, missing_power=None, missing_route=False, shared_standby=False,
                 failed_add=None):
        super().__init__(failed_add=failed_add)
        self.missing_power = missing_power
        self.missing_route = missing_route
        self.shared_standby = shared_standby
        self.actions = []

    def scan(self, main):
        if tuple(self.members) != self.member_duts:
            raise AssertionError("The headless owner must be added before scanning")
        fixture = self

        class Scan:
            def assert_contains(self, *components):
                fixture.events.append(("scan.contains", tuple(components)))

            def assert_contains_only(self, *components):
                fixture.events.append(("scan.only", tuple(components)))

            def assert_on(self, component, endpoint):
                if component.endpoint != endpoint:
                    raise AssertionError("Scan evidence must identify the exact owner, adapter and side")
                fixture.events.append(("scan.on", component, endpoint))

        return Scan()

    def activity(self, main, name, *, display, audio, source):
        fixture = self

        class Activity:
            components = frozenset((display, audio, source))

            def __enter__(self):
                if main.name != "amity-dut" or fixture.current is not None:
                    raise AssertionError("Create activities through the main while in standby")
                fixture.activities.append(self)
                fixture.events.append(("activity.create", name))
                return self

            def start(self):
                previous = fixture.current
                old = previous.components if previous else frozenset()
                commands = {component: PowerState.STANDBY for component in old - self.components}
                commands.update({component: PowerState.ON for component in self.components - old})
                for component, power in commands.items():
                    component._power = power
                action = SimpleNamespace(kind="start", display=display, previous=previous,
                                         commands=commands, power_checks=[], state_checks=[], route_checked=False,
                                         route=not fixture.missing_route, audio=audio, source=source)
                if previous and fixture.missing_power:
                    omitted_display, omitted_power = fixture.missing_power
                    action.commands = {component: power for component, power in commands.items()
                                       if (component.name, power) != (omitted_display, omitted_power)}
                if previous and fixture.shared_standby:
                    audio._power = PowerState.STANDBY
                source.active = True
                audio._active_input = source.address.physical
                fixture.current = self
                fixture.actions.append(action)
                return action

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The selected activity changed")
                fixture.events.append(("activity.active", display))

            def standby(self):
                if fixture.current is not self:
                    raise AssertionError("Stop the selected activity")
                commands = {component: PowerState.STANDBY for component in self.components}
                for component, power in commands.items():
                    component._power = power
                source.active = False
                action = SimpleNamespace(kind="standby", display=display, commands=commands,
                                         power_checks=[], state_checks=[])
                fixture.actions.append(action)
                fixture.current = None
                return action

            def __exit__(self, *args):
                fixture.activities.remove(self)
                fixture.events.append(("activity.remove", name))

        return Activity()

    def assert_power(self, component, power):
        result = super().assert_power(component, power)
        if self.actions:
            self.actions[-1].state_checks.append((component, power))
        return result

    def wait_power(self, component, power, *, since, command=None, directed=False):
        if since not in self.actions or since.commands.get(component) != power:
            raise AssertionError("Missing fresh physical power command")
        if power == PowerState.STANDBY and command != "standby":
            raise AssertionError("Off requires an actual Standby command")
        self.assert_power(component, power)
        since.power_checks.append((component, power, command, directed))
        return component

    def route(self, avr, player, *, since):
        if (since not in self.actions or since.kind != "start" or not since.route
                or avr is not since.audio or player is not since.source):
            raise AssertionError("Missing fresh physical Set Stream Path")
        since.route_checked = True
        return avr

    def run(self):
        with ExitStack() as patches:
            patches.enter_context(patch.object(dual_display, "TestBed", self.bed))
            patches.enter_context(patch.object(DUT, "add_amity", lambda main, candidate: self.add(main, candidate)))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda main: self.scan(main)))
            patches.enter_context(patch.object(DUT, "create_activity", lambda main, name, **kw: self.activity(main, name, **kw)))
            patches.enter_context(patch.object(Component, "assert_power", lambda component, power: self.assert_power(component, power)))
            patches.enter_context(patch.object(Component, "wait_for_power", lambda component, power, **kw: self.wait_power(component, power, **kw)))
            patches.enter_context(patch.object(Component, "wait_for_input", lambda avr, player, **kw: self.route(avr, player, **kw)))
            dual_display.test_dual_display()


class DualDisplayScenarioTests(unittest.TestCase):
    def assert_cleanup(self, scenario):
        cleanup = [row[0] for row in scenario.events if row[0] in {"activity.remove", "member.remove", "bed.close"}]
        self.assertEqual(cleanup, ["activity.remove", "activity.remove", "member.remove", "bed.close"])
        self.assertEqual(scenario.members, [])
        self.assertEqual(scenario.activities, [])

    def test_exact_topology_and_both_standalone_and_direct_activity_changes(self):
        scenario = DualDisplayScenario()
        scenario.run()
        main, secondary = scenario.duts
        self.assertEqual((main.name, secondary.name), ("amity-dut", "amity-dut-4"))
        self.assertEqual(scenario.member_duts, (secondary,))
        self.assertEqual(len(scenario.components), 4)
        tv, projector = sorted((c for c in scenario.components if isinstance(c, TvComponent)),
                               key=lambda c: c.tentacle)
        avr, = (c for c in scenario.components if isinstance(c, AvrComponent))
        player, = (c for c in scenario.components if isinstance(c, PlaybackComponent))
        self.assertEqual(tv.endpoint, secondary.tentacle(1).front)
        self.assertEqual(projector.endpoint, secondary.tentacle(2).front)
        self.assertEqual(avr.endpoint, secondary.tentacle(1).back)
        self.assertIs(player.parent, avr)
        self.assertEqual(player.port, 1)
        self.assertEqual({c.gpio for c in scenario.components},
                         {tv.gpio, avr.gpio, projector.gpio})
        self.assertNotIn(secondary.tentacle(2).back.gpio, {c.gpio for c in scenario.components})
        self.assertEqual((tv.address.physical, projector.address.physical,
                          avr.address.physical, player.address.physical), (0, 0, 0x1000, 0x1100))
        for kind in ("scan.contains", "scan.only"):
            self.assertEqual([row[1] for row in scenario.events if row[0] == kind], [scenario.components])
        self.assertEqual({row[1:] for row in scenario.events if row[0] == "scan.on"},
                         {(c, c.endpoint) for c in scenario.components})
        starts = [action for action in scenario.actions if action.kind == "start"]
        self.assertEqual({(a.previous is None, a.display) for a in starts},
                         {(True, tv), (True, projector), (False, tv), (False, projector)})
        for action in starts:
            self.assertTrue(action.route_checked)
            checks = {(c, power) for c, power, _, _ in action.power_checks}
            if action.previous is None:
                self.assertEqual(checks, {(c, PowerState.ON) for c in (action.display, avr, player)})
                excluded = projector if action.display is tv else tv
                self.assertIn((excluded, PowerState.STANDBY), action.state_checks)
            else:
                excluded = projector if action.display is tv else tv
                self.assertEqual(checks, {(action.display, PowerState.ON), (excluded, PowerState.STANDBY)})
                self.assertIn((excluded, PowerState.STANDBY, "standby", True), action.power_checks)
                for shared in (avr, player):
                    self.assertIn((shared, PowerState.ON), action.state_checks)
        stops = [action for action in scenario.actions if action.kind == "standby"]
        self.assertEqual({action.display for action in stops}, {tv, projector})
        for action in stops:
            self.assertEqual({c for c, _, command, _ in action.power_checks if command == "standby"},
                             {action.display, avr, player})
        self.assertEqual([row for row in scenario.events if row[0] == "member.connected"],
                         [("member.connected", secondary.name)] * 2)
        self.assert_cleanup(scenario)

    def test_missing_switch_commands_fail_even_if_local_power_state_is_correct(self):
        for name in ("Dual TV", "Dual Projector"):
            for power in (PowerState.ON, PowerState.STANDBY):
                with self.subTest(name=name, power=power):
                    scenario = DualDisplayScenario(missing_power=(name, power))
                    with self.assertRaisesRegex(AssertionError, "Missing fresh physical power command"):
                        scenario.run()
                    self.assert_cleanup(scenario)

    def test_shared_avr_cannot_enter_standby_during_display_handoff(self):
        scenario = DualDisplayScenario(shared_standby=True)
        with self.assertRaisesRegex(AssertionError, "Unexpected Dual AVR power"):
            scenario.run()
        self.assert_cleanup(scenario)

    def test_missing_route_fails_even_if_model_input_matches(self):
        scenario = DualDisplayScenario(missing_route=True)
        with self.assertRaisesRegex(AssertionError, "Missing fresh physical Set Stream Path"):
            scenario.run()
        self.assert_cleanup(scenario)

    def test_failed_admission_closes_bed_without_touching_other_members(self):
        scenario = DualDisplayScenario(failed_add="amity-dut-4")
        with self.assertRaisesRegex(RuntimeError, "candidate addition failed"):
            scenario.run()
        self.assertEqual(scenario.events[-1], ("bed.close",))
        self.assertFalse(any(row[0] in {"member.remove", "activity.create"} for row in scenario.events))


if __name__ == "__main__":
    unittest.main()
