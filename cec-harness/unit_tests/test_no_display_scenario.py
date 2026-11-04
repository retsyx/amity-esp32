"""A display-free activity must retire its prior TV and retain back-side control."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, Component, DUT, Key, PlaybackComponent, PowerState, TvComponent
from functional_tests import activity as scenarios
from unit_tests.test_consortium_scenarios import Scenario


class NoDisplayScenario(Scenario):
    def __init__(self, *, fault=None):
        super().__init__()
        self.fault, self.actions, self.observations = fault, [], []
        self.unused = None

    @contextmanager
    def scanned(self):
        dut = DUT("amity-dut")
        connector = dut.tentacle(0)
        self.tv = TvComponent(connector.front, "Audio TV")
        self.avr = AvrComponent(connector.back, "Audio AVR")
        self.player = PlaybackComponent(self.avr, "Audio Player", port=1)
        dut.last_scan = object()
        with self.bed(dut, self.tv, self.avr, self.player, members=()):
            yield dut, self.tv, self.avr, self.player

    def activity(self, dut, name, *, display, audio, source):
        fixture = self

        class Activity:
            def __init__(self):
                self.display = display
                self.selected = frozenset(component for component in (display, audio, source) if component is not None)

            def __enter__(self):
                if fixture.current is not None:
                    raise AssertionError("Save both activities before playback")
                fixture.activities.append(self)
                fixture.events.append(("activity.create", self))
                return self

            def start(self):
                previous = fixture.current
                kind = "repair" if previous is self else "handoff" if previous else "start"
                if kind == "repair" and (source.power != PowerState.STANDBY or audio.active_input != 0x1200):
                    raise AssertionError("Repair needs disturbed back-side equipment")
                old = previous.selected if previous else frozenset()
                commands = {component: PowerState.STANDBY for component in old - self.selected}
                commands.update({component: PowerState.ON for component in
                                 (self.selected if kind == "repair" else self.selected - old)})
                for component, power in commands.items():
                    component._power = power
                audio._active_input = source.address.physical
                source.active = True
                if display is not None:
                    display._active_input = display.tv_input << 12
                action = SimpleNamespace(kind=kind, activity=self, previous=previous, commands=commands,
                                         directed=True, route=True, display=display,
                                         display_input=display.tv_input << 12 if display else None)
                # Final state stays correct when command evidence is absent.
                if fixture.fault == "first power" and kind == "start":
                    action.commands.pop(display)
                elif fixture.fault == "first input" and kind == "start":
                    action.display_input = None
                elif fixture.fault == "wrong first input" and kind == "start":
                    action.display_input = 0x2000
                elif fixture.fault == "TV standby" and kind == "handoff":
                    action.commands.pop(fixture.tv)
                elif fixture.fault == "broadcast TV standby" and kind == "handoff":
                    action.directed = False
                elif fixture.fault == "repair power" and kind == "repair":
                    action.commands.pop(source)
                if fixture.fault == kind + " route":
                    action.route = False
                fixture.current = self
                fixture.actions.append(action)
                fixture.events.append(("activity.start", kind, self))
                return action

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The selected activity changed")
                fixture.events.append(("activity.active", self))

            def standby(self):
                commands = {component: PowerState.STANDBY for component in self.selected}
                for component, power in commands.items():
                    component._power = power
                source.active = False
                action = SimpleNamespace(kind="standby", activity=self, commands=commands)
                fixture.actions.append(action)
                fixture.current = None
                fixture.events.append(("activity.standby", self))
                return action

            def __exit__(self, *args):
                if fixture.current is self:
                    fixture.current = None
                fixture.activities.remove(self)
                fixture.events.append(("activity.remove", self))

        return Activity()

    def wait_power(self, component, power, *, since, command=None, directed=False):
        if since not in self.actions or since.commands.get(component) != power:
            raise AssertionError("Missing fresh physical power command")
        if power == PowerState.STANDBY and command != "standby":
            raise AssertionError("Require the actual Standby command")
        if component is self.tv and power == PowerState.STANDBY and (not directed or not since.directed):
            raise AssertionError("The old TV needs directed Standby evidence")
        self.assert_power(component, power)
        self.events.append(("power.wait", since.kind, component))
        return component

    def display_route(self, display, *, since):
        if since not in self.actions or since.display is not display or since.display_input != display.tv_input << 12:
            raise AssertionError("Missing fresh physical TV input announcement")
        self.events.append(("display.wait", since))
        return display

    def route(self, avr, source, *, since):
        if since not in self.actions or not since.route or avr is not self.avr or source is not self.player:
            raise AssertionError("Missing fresh physical back-side routing")
        self.events.append(("route.wait", since))
        return avr

    @contextmanager
    def preserve(self, *components):
        fixture = self

        class Observation:
            checked = False

            def assert_preserved(self, *, since):
                if since not in fixture.actions or since.kind not in {"handoff", "repair"}:
                    raise AssertionError("Preservation must use the actual transition or repair")
                if fixture.fault == "shared standby":
                    raise AssertionError("Shared equipment received transient Standby")
                self.checked = True
                fixture.events.append(("shared.preserved", since))

            def assert_preserved_after(self, announcement, *, within):
                if announcement is not fixture.announcement or within != .250:
                    raise AssertionError("Observe the actual selected source announcement")
                if fixture.fault == "announcement standby":
                    raise AssertionError("Selected source received transient Standby")
                self.checked = True
                fixture.events.append(("announcement.preserved", announcement))

        if set(components) != {self.avr, self.player}:
            raise AssertionError("Only back-side devices are shared by the no-display activity")
        observation = Observation()
        self.observations.append(observation)
        self.events.append(("shared.observe",))
        try:
            yield observation
        except BaseException:
            raise
        else:
            if not observation.checked:
                raise AssertionError("The preservation observation was never checked")
        finally:
            self.observations.remove(observation)

    @contextmanager
    def handoff(self, display):
        fixture = self
        if self.current is None or self.current.display is not display:
            raise AssertionError("Observe the TV before changing activities")

        class Observation:
            def assert_standby_only(self, *, since):
                if since.kind != "handoff" or fixture.unused is None:
                    raise AssertionError("Overlap the TV transition and no-control observations")
                if fixture.fault == "transition TV control":
                    raise AssertionError("The TV received hidden handoff control")
                fixture.events.append(("handoff.preserved", since))

        self.events.append(("handoff.observe", display))
        yield Observation()

    @contextmanager
    def observe_unused(self, scan, display):
        fixture = self
        if self.current.display is not None or display is not self.tv:
            raise AssertionError("Observe only the retired TV during audio playback")

        class Observation:
            def assert_clear(self):
                if fixture.fault == "playback TV control":
                    raise AssertionError("The unused TV received hidden playback control")
                fixture.events.append(("TV.unused",))

        self.unused = Observation()
        self.events.append(("TV.observe",))
        try:
            yield self.unused
        finally:
            self.unused = None

    def key(self, dut, key):
        fixture = self

        class Action:
            def assert_received_by(self, recipient):
                expected = fixture.player if key == Key.SELECT else fixture.avr if key == Key.VOLUME_UP else None
                if fixture.fault == "key" or recipient is not expected or fixture.current.display is not None:
                    raise AssertionError("Missing fresh key on the audio activity's receiver")
                fixture.events.append(("key.received", key, recipient))
                if (fixture.fault == "final key selection"
                        and sum(row[0] == "key.received" for row in fixture.events) == 4):
                    fixture.current = fixture.activities[0]

        return Action()

    def activate(self, player):
        if player is not self.player or self.current.display is not None:
            raise AssertionError("Announce the current audio activity's source")
        self.announcement = SimpleNamespace(ok=True)
        self.events.append(("source.activate",))
        if self.fault == "announcement selection":
            self.current = self.activities[0]
        return self.announcement

    def disturb(self, component):
        component._power = PowerState.STANDBY
        self.events.append(("fixture.standby", component))

    def run(self):
        with ExitStack() as patches:
            for target, name, replacement in (
                (scenarios, "scanned_devices", self.scanned),
                (scenarios, "ActivityDisplayStandby", self.handoff),
                (scenarios, "observe_no_standby", self.preserve),
                (scenarios, "observe_no_control", self.observe_unused),
                (DUT, "create_activity", lambda dut, name, **kw: self.activity(dut, name, **kw)),
                (DUT, "press_key", self.key),
                (Component, "assert_power", self.assert_power),
                (Component, "wait_for_power", self.wait_power),
                (Component, "wait_for_input", self.route),
                (TvComponent, "wait_for_amity_input", self.display_route),
                (Component, "standby", self.disturb),
                (PlaybackComponent, "activate", self.activate),
            ):
                # Bound fixture methods need the component/DUT supplied by the
                # patched descriptor, rather than binding to the fixture again.
                patches.enter_context(patch.object(target, name, lambda *args, method=replacement, **kw: method(*args, **kw)))
            scenarios.test_activity_no_display()


class NoDisplayScenarioTests(unittest.TestCase):
    def assert_cleanup(self, fixture):
        self.assertFalse(fixture.activities)
        self.assertFalse(fixture.observations)
        self.assertIsNone(fixture.unused)
        self.assertIsNone(fixture.current)
        self.assertEqual(fixture.events[-1], ("bed.close",))

    def test_direct_handoff_and_audio_repair_retain_back_side_control_without_the_tv(self):
        fixture = NoDisplayScenario()
        fixture.run()
        self.assertEqual([action.kind for action in fixture.actions], ["start", "handoff", "repair", "standby"])
        self.assertEqual([action.display for action in fixture.actions[:3]], [fixture.tv, None, None])
        self.assertEqual(len([row for row in fixture.events if row[0] == "display.wait"]), 1)
        self.assertEqual([row[1].kind for row in fixture.events if row[0] == "route.wait"],
                         ["start", "handoff", "repair"])
        self.assertEqual([row[2] for row in fixture.events if row[:2] == ("power.wait", "handoff")], [fixture.tv])
        self.assertEqual({row[2] for row in fixture.events if row[:2] == ("power.wait", "repair")},
                         {fixture.avr, fixture.player})
        self.assertEqual([(row[1], row[2]) for row in fixture.events if row[0] == "key.received"],
                         [(Key.SELECT, fixture.player), (Key.VOLUME_UP, fixture.avr)] * 2)
        kinds = [row[0] for row in fixture.events]
        self.assertLess(kinds.index("handoff.observe"), kinds.index("TV.observe"))
        self.assertLess(kinds.index("TV.observe"), kinds.index("handoff.preserved"))
        self.assertLess(kinds.index("source.activate"), kinds.index("announcement.preserved"))
        self.assertLess(kinds.index("TV.unused"), kinds.index("activity.standby"))
        self.assertEqual(fixture.tv.power, PowerState.STANDBY)
        self.assert_cleanup(fixture)

    def test_missing_evidence_and_hidden_tv_control_fail_even_when_final_model_state_is_correct(self):
        faults = ("first power", "first input", "wrong first input", "start route", "TV standby",
                  "broadcast TV standby", "handoff route", "transition TV control", "playback TV control",
                  "shared standby", "announcement standby", "repair power", "repair route", "key",
                  "announcement selection", "final key selection")
        for fault in faults:
            with self.subTest(fault=fault):
                fixture = NoDisplayScenario(fault=fault)
                message = "The selected activity changed" if fault.endswith(" selection") else ".*"
                with self.assertRaisesRegex(AssertionError, message):
                    fixture.run()
                if fault == "announcement selection":
                    self.assertNotIn("repair", [action.kind for action in fixture.actions])
                self.assert_cleanup(fixture)


if __name__ == "__main__":
    unittest.main()
