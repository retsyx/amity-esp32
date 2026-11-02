"""Activity repair and disconnected-hold scenarios require their evidence helpers."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, Component, DUT, Key, PlaybackComponent,
                           PowerState, TvComponent)
from functional_tests import activity as scenarios
from unit_tests.test_consortium_scenarios import Scenario


class ActivityScenario(Scenario):
    def __init__(self, *, missing_repair_power=None, missing_repair_route=False,
                 repair_standby=False, disconnect_failure=False, missing_followup=False):
        super().__init__()
        self.missing_repair_power = missing_repair_power
        self.missing_repair_route = missing_repair_route
        self.repair_standby = repair_standby
        self.disconnect_failure = disconnect_failure
        self.missing_followup = missing_followup
        self.actions = []
        self.observation = None

    @contextmanager
    def scanned(self):
        dut = DUT("amity-dut")
        connection = dut.tentacle(0)
        tv = TvComponent(connection.front, "Scenario TV")
        avr = AvrComponent(connection.back, "Scenario AVR")
        player = PlaybackComponent(avr, "Trial Player", port=1)
        with self.bed(dut, tv, avr, player, members=()):
            yield dut, tv, avr, player

    def activity(self, main, name, *, display, audio, source):
        fixture = self
        components = (display, audio, source)

        class Activity:
            def __enter__(self):
                if fixture.current is not None:
                    raise AssertionError("Create the fixture activity in Standby")
                fixture.activities.append(self)
                fixture.events.append(("activity.create", self, name))
                return self

            def start(self):
                repair = fixture.current is self
                if fixture.current is not None and not repair:
                    raise AssertionError("Repair must reselect the exact saved activity")
                if repair:
                    if source.power != PowerState.STANDBY or audio.active_input != 0x1200:
                        raise AssertionError("Repair must begin with disturbed fixture power and routing")
                    if fixture.observation is None:
                        raise AssertionError("Repair needs continuous no-Standby observation")
                    fixture.observation.saw_standby = fixture.repair_standby
                # Model state is correct even when physical command evidence is
                # deliberately missing: assertions must not pass from state alone.
                for component in components:
                    component._power = PowerState.ON
                source.active = True
                audio._active_input = source.address.physical
                power_commands = set(components)
                if repair and fixture.missing_repair_power is not None:
                    power_commands.discard(components[fixture.missing_repair_power])
                action = SimpleNamespace(kind="repair" if repair else "start", activity=self,
                                         owner=source.dut.name, power_commands=power_commands,
                                         route=not (repair and fixture.missing_repair_route),
                                         source=source, audio=audio)
                fixture.current = self
                fixture.actions.append(action)
                fixture.events.append(("activity.start", self, action.kind))
                return action

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The selected activity changed")
                fixture.events.append(("activity.active", self))

            def standby(self):
                if fixture.current is not self:
                    raise AssertionError("Stop the exact selected activity")
                for component in components:
                    component._power = PowerState.STANDBY
                source.active = False
                fixture.current = None
                action = SimpleNamespace(kind="standby", activity=self, owner=source.dut.name,
                                         power_commands=set(components))
                fixture.actions.append(action)
                fixture.events.append(("activity.standby", self))
                return action

            def __exit__(self, *args):
                fixture.current = None
                fixture.activities.remove(self)
                fixture.events.append(("activity.remove", self))

        return Activity()

    def wait_power(self, component, power, *, since, command=None, directed=False):
        if since not in self.actions or component not in since.power_commands:
            raise AssertionError("Missing fresh physical power command")
        if power == PowerState.STANDBY and command != "standby":
            raise AssertionError("Standby requires a fresh Standby command")
        self.assert_power(component, power)
        self.events.append(("power.wait", since.kind, component))
        return component

    def route(self, avr, player, *, since):
        if (since not in self.actions or not since.route or since.audio is not avr
                or since.source is not player):
            raise AssertionError("Missing fresh physical routing command")
        self.events.append(("route.wait", since.kind, avr, player))
        return avr

    def disturb(self, component):
        component._power = PowerState.STANDBY
        self.events.append(("fixture.standby", component))

    def preserve(self, *components):
        fixture = self

        class Observation:
            def __enter__(self):
                fixture.observation = self
                self.saw_standby = self.checked = False
                fixture.events.append(("preserve.start", tuple(components)))
                return self

            def assert_preserved(self, *, since):
                if since.kind != "repair" or since.activity is not fixture.current:
                    raise AssertionError("Preservation evidence needs this exact repair")
                if self.saw_standby:
                    raise AssertionError("Repair emitted an unexpected physical Standby")
                self.checked = True
                fixture.events.append(("preserve.checked", since))

            def __exit__(self, exc_type, exc, traceback):
                fixture.observation = None
                if exc_type is None and not self.checked:
                    raise AssertionError("No-Standby observation was never asserted")
                fixture.events.append(("preserve.close",))

        return Observation()

    def disconnected(self, main, key, recipient):
        if fixture_owner(self.current, self.actions) != recipient.dut.name or key != Key.VOLUME_UP:
            raise AssertionError("Disconnect cleanup must target the active owner's AVR")
        self.events.append(("key.disconnected", main, key, recipient))
        if self.disconnect_failure:
            raise TimeoutError("Disconnected hold did not release")

    def key(self, main, key, **kwargs):
        fixture = self

        class Action:
            def assert_received_by(self, recipient):
                if (fixture.missing_followup or key != Key.SELECT
                        or recipient is not fixture.actions[-1].source):
                    raise AssertionError("Missing fresh follow-up key evidence")
                fixture.events.append(("key.followup", main, key, recipient))
                return self

        fixture.events.append(("key.request", main, key))
        return Action()

    def run(self, test=scenarios.test_activity):
        with ExitStack() as patches:
            patches.enter_context(patch.object(scenarios, "scanned_devices", self.scanned))
            patches.enter_context(patch.object(scenarios, "TestBed", self.bed))
            patches.enter_context(patch.object(scenarios, "observe_no_standby", self.preserve))
            patches.enter_context(patch.object(DUT, "add_amity", lambda main, candidate: self.add(main, candidate)))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda main: self.scan(main)))
            patches.enter_context(patch.object(DUT, "create_activity", lambda main, name, **kw: self.activity(main, name, **kw)))
            patches.enter_context(patch.object(DUT, "press_key_disconnected", lambda main, key, recipient: self.disconnected(main, key, recipient)))
            patches.enter_context(patch.object(DUT, "press_key", lambda main, key, **kw: self.key(main, key, **kw)))
            patches.enter_context(patch.object(Component, "assert_power", lambda component, power: self.assert_power(component, power)))
            patches.enter_context(patch.object(Component, "wait_for_power", lambda component, power, **kw: self.wait_power(component, power, **kw)))
            patches.enter_context(patch.object(Component, "wait_for_input", lambda avr, player, **kw: self.route(avr, player, **kw)))
            patches.enter_context(patch.object(Component, "standby", lambda component: self.disturb(component)))
            test()


def fixture_owner(current, actions):
    return next(action.owner for action in reversed(actions) if action.activity is current)


class ActivityScenarioTests(unittest.TestCase):
    def assert_cleanup(self, fixture):
        self.assertFalse(fixture.activities)
        self.assertFalse(fixture.members)
        self.assertIsNone(fixture.observation)
        self.assertEqual(fixture.events[-1], ("bed.close",))

    def test_repair_reselects_same_saved_activity_after_power_and_input_disturbance(self):
        fixture = ActivityScenario()
        fixture.run()
        creates = [row for row in fixture.events if row[0] == "activity.create"]
        starts = [row for row in fixture.events if row[0] == "activity.start"]
        self.assertEqual(len(creates), 1)
        self.assertEqual([row[2] for row in starts], ["start", "repair"])
        self.assertIs(starts[0][1], starts[1][1])
        self.assertIs(starts[0][1], creates[0][1])
        player, = (c for c in fixture.components if isinstance(c, PlaybackComponent))
        self.assertIn(("fixture.standby", player), fixture.events)
        self.assertIn(("preserve.start", fixture.components), fixture.events)
        self.assertEqual(len([row for row in fixture.events if row[0] == "preserve.checked"]), 1)
        for kind in ("start", "repair", "standby"):
            self.assertEqual({row[2] for row in fixture.events if row[:2] == ("power.wait", kind)},
                             set(fixture.components))
        self.assertEqual([row[1] for row in fixture.events if row[0] == "route.wait"], ["start", "repair"])
        self.assert_cleanup(fixture)

    def test_missing_repair_power_fails_even_when_final_model_power_is_on(self):
        for recipient in range(3):
            with self.subTest(recipient=recipient):
                fixture = ActivityScenario(missing_repair_power=recipient)
                with self.assertRaisesRegex(AssertionError, "Missing fresh physical power"):
                    fixture.run()
                self.assertTrue(all(c.power == PowerState.ON for c in fixture.components))
                self.assert_cleanup(fixture)

    def test_missing_repair_route_fails_even_when_final_model_input_matches(self):
        fixture = ActivityScenario(missing_repair_route=True)
        with self.assertRaisesRegex(AssertionError, "Missing fresh physical routing"):
            fixture.run()
        avr, = (c for c in fixture.components if isinstance(c, AvrComponent))
        player, = (c for c in fixture.components if isinstance(c, PlaybackComponent))
        self.assertEqual(avr.active_input, player.address.physical)
        self.assert_cleanup(fixture)

    def test_transient_repair_standby_fails_even_when_final_model_power_is_on(self):
        fixture = ActivityScenario(repair_standby=True)
        with self.assertRaisesRegex(AssertionError, "unexpected physical Standby"):
            fixture.run()
        self.assertTrue(all(c.power == PowerState.ON for c in fixture.components))
        self.assert_cleanup(fixture)

    def test_local_and_member_disconnected_holds_require_cleanup_and_followup_key(self):
        for test, owner in ((scenarios.test_activity_keys_disconnected, "amity-dut"),
                            (scenarios.test_activity_keys_disconnected_secondary, "amity-dut-2")):
            with self.subTest(owner=owner):
                fixture = ActivityScenario()
                fixture.run(test)
                disconnected, = (row for row in fixture.events if row[0] == "key.disconnected")
                followup, = (row for row in fixture.events if row[0] == "key.followup")
                self.assertEqual(disconnected[1].name, "amity-dut")
                self.assertEqual((disconnected[2], disconnected[3].dut.name), (Key.VOLUME_UP, owner))
                self.assertIsInstance(disconnected[3], AvrComponent)
                self.assertEqual((followup[2], followup[3].dut.name), (Key.SELECT, owner))
                self.assertIsInstance(followup[3], PlaybackComponent)
                self.assertLess(fixture.events.index(disconnected), fixture.events.index(followup))
                self.assertEqual(len([row for row in fixture.events if row[:2] == ("power.wait", "standby")]), 3)
                if owner != "amity-dut":
                    self.assertEqual([row for row in fixture.events if row[0] == "member.connected"],
                                     [("member.connected", owner)] * 2)
                    self.assertEqual(len([row for row in fixture.events if row[0] == "scan.on"]), 3)
                self.assert_cleanup(fixture)

    def test_disconnected_cleanup_or_followup_failure_propagates_and_closes_resources(self):
        for test in (scenarios.test_activity_keys_disconnected, scenarios.test_activity_keys_disconnected_secondary):
            for kwargs, error, message in (({"disconnect_failure": True}, TimeoutError, "did not release"),
                                           ({"missing_followup": True}, AssertionError, "follow-up key")):
                with self.subTest(test=test.__name__, **kwargs):
                    fixture = ActivityScenario(**kwargs)
                    with self.assertRaisesRegex(error, message):
                        fixture.run(test)
                    self.assert_cleanup(fixture)


if __name__ == "__main__":
    unittest.main()
