"""BLE scenarios keep pairing ownership and require every physical input proof."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Component, DUT, PowerState
from functional_tests import ble_keyboard
from unit_tests.test_consortium_scenarios import Scenario


class KeyboardScenario(Scenario):
    def __init__(self, *, existing_bond=False, failed_proof=None):
        super().__init__()
        self.existing_bond, self.failed_proof = existing_bond, failed_proof
        self.keyboard = self.scan_proof = None

    @contextmanager
    def pair(self, main):
        if main is not self.duts[0] or main.name != "amity-dut" or not self.open:
            raise AssertionError("Only the original main owns the keyboard")
        self.events.append(("keyboard.review", main.name))
        if self.existing_bond:
            raise RuntimeError("Existing remote bond must be preserved")
        self.keyboard = SimpleNamespace(main=main, connected=True)
        self.events.append(("keyboard.pair", main.name))
        try:
            yield self.keyboard
        finally:
            if self.activities or self.members:
                raise AssertionError("Close activities and remove members before keyboard unpair")
            self.events.append(("keyboard.unpair", main.name))
            self.keyboard = None

    def add(self, main, candidate):
        if self.keyboard is None:
            raise AssertionError("Reject an existing keyboard bond before adding fixture members")
        return super().add(main, candidate)

    def scan(self, main):
        if self.keyboard is None:
            raise AssertionError("Pair the real-input fixture before changing HDMI discovery")
        self.events.append(("scan",))
        self.scan_proof = super().scan(main)
        return self.scan_proof

    @contextmanager
    def activity(self, main, name, *, display, audio, source):
        roles = (display, audio, source)
        components = tuple(component for component in roles if component is not None)
        owner = source.dut.name
        activity = SimpleNamespace(components=roles, name=name)

        def start():
            if len(self.activities) != 2 or self.current is not None:
                raise AssertionError("Save both activities before input and finish standby before restart")
            self.current = activity
            for component in components:
                component._power = PowerState.ON
            source.active = True
            if audio is not None:
                audio._active_input = source.address.physical
            self.events.append(("activity.start", owner, name))
            return SimpleNamespace(kind="start", owner=owner)

        def standby():
            if self.current is not activity:
                raise AssertionError("Standby must stop the current fixture activity")
            self.current = None
            for component in components:
                component._power = PowerState.STANDBY
            source.active = False
            self.events.append(("activity.standby", owner, name))
            return SimpleNamespace(kind="standby", owner=owner)

        def active():
            if self.current is not activity:
                raise AssertionError("The selected fixture activity changed")
            self.events.append(("activity.active", owner, name))
            return activity

        activity.start, activity.standby, activity.assert_active = start, standby, active
        self.activities.append(activity)
        self.events.append(("activity.create", owner, name, roles))
        try:
            yield activity
        finally:
            if self.current is activity:
                standby()
            self.activities.remove(activity)
            self.events.append(("activity.remove", owner, name))

    @contextmanager
    def evidence(self, main, scan, *components):
        if main is not self.duts[0] or scan is not self.scan_proof or components != self.components:
            raise AssertionError("Input evidence needs the exact scanned owner and components")
        self.events.append(("input.observe",))
        fixture = self

        class Evidence:
            def check(self, kind, keyboard, *recipients):
                if keyboard is not fixture.keyboard or not keyboard.connected or fixture.current is None:
                    raise AssertionError("The real keyboard must control the current fixture activity")
                fixture.events.append(("input." + kind, *recipients))
                if fixture.failed_proof == kind:
                    raise TimeoutError("Missing fresh " + kind + " physical evidence")

            def assert_tap(self, keyboard, player):
                if player is not fixture.components[2]:
                    raise AssertionError("Keyboard Enter must reach the selected player")
                self.check("tap", keyboard, player)

            def assert_overlapping_buttons(self, keyboard, *, volume, navigation):
                if volume is not fixture.components[1] or navigation is not fixture.components[2]:
                    raise AssertionError("Independent volume and navigation need their actual AVR/player owners")
                self.check("overlap", keyboard, volume, navigation)

            def assert_shared_recipient(self, keyboard, player):
                if player is not fixture.components[2]:
                    raise AssertionError("Shared navigation buttons must reach the first selected player")
                self.check("shared", keyboard, player)

            def assert_failed_press(self, keyboard, player, *, after_repetition=False):
                if player is not fixture.components[2]:
                    raise AssertionError("Physical NACKs must exercise the first selected player")
                self.check("failed_repeat" if after_repetition else "failed_press", keyboard, player)

            def assert_disconnect_cleanup(self, keyboard, avr):
                if avr is not fixture.components[1]:
                    raise AssertionError("Disconnect cleanup must release the held AVR key")
                if fixture.current.components != (fixture.components[0], avr, fixture.components[2]):
                    raise AssertionError("Restore the original full activity before disconnect cleanup")
                self.check("disconnect", keyboard, avr)
                keyboard.connected = False

        try:
            yield Evidence()
        finally:
            self.events.append(("input.close",))

    @contextmanager
    def transition_evidence(self, main, scan, *components):
        if main is not self.duts[0] or scan is not self.scan_proof or components != self.components:
            raise AssertionError("Transitions need all four exact scanned components")
        self.events.append(("transition.observe",))
        fixture = self

        class Evidence:
            def assert_transition(self, keyboard, *, old_source, source, display, old_display, routing, volume, activity, all_up):
                if (keyboard is not fixture.keyboard or not keyboard.connected or fixture.current is None
                        or old_source is not fixture.current.components[2]
                        or activity.components != (None, None, source) or display is not None
                        or old_display is not (None if all_up else fixture.components[0])
                        or routing is not fixture.components[1]
                        or volume is not (None if all_up else fixture.components[1])
                        or source is not fixture.components[3]):
                    raise AssertionError("BLE activity selection needs the exact old and next recipients")
                kind = "all_up" if all_up else "fallback"
                if (not all_up and old_source is not fixture.components[2]
                        or all_up and old_source is not source):
                    raise AssertionError("Exercise a source change and accepted same-target selection")
                fixture.events.append(("transition." + kind, old_source, source, routing, volume))
                if fixture.failed_proof == "transition_" + kind:
                    raise TimeoutError("Missing fresh transition_" + kind + " physical evidence")
                for component in fixture.current.components:
                    if component is not None and component not in activity.components:
                        component._power = PowerState.STANDBY
                for component in activity.components:
                    if component is not None:
                        component._power = PowerState.ON
                source.active = True
                routing._active_input = source.address.physical
                fixture.current = activity

        try:
            yield Evidence()
        finally:
            self.events.append(("transition.close",))

    def run(self, test):
        with ExitStack() as patches:
            replacements = (
                (ble_keyboard, "TestBed", self.bed),
                (ble_keyboard, "BLEInputEvidence", self.evidence),
                (ble_keyboard, "BLEActivityEvidence", self.transition_evidence),
                (DUT, "pair_ble_keyboard", lambda main: self.pair(main)),
                (DUT, "add_amity", lambda main, candidate: self.add(main, candidate)),
                (DUT, "scan_hdmi", lambda main: self.scan(main)),
                (DUT, "create_activity", lambda main, name, **kwargs: self.activity(main, name, **kwargs)),
                (Component, "wait_for_power", lambda component, power, **kwargs: self.wait_power(component, power, **kwargs)),
                (Component, "wait_for_input", lambda avr, player, **kwargs: self.route(avr, player, **kwargs)),
                (Component, "assert_power", lambda component, power: self.assert_power(component, power)),
            )
            for target, name, replacement in replacements:
                patches.enter_context(patch.object(target, name, replacement))
            test()


class BLEKeyboardScenarioTests(unittest.TestCase):
    cases = ((ble_keyboard.test_ble_keyboard_local, False), (ble_keyboard.test_ble_keyboard_remote, True))

    def assert_cleanup(self, scenario, remote):
        events = [event[0] for event in scenario.events]
        self.assertLess(events.index("activity.remove"), events.index("keyboard.unpair"))
        self.assertEqual(events.count("activity.remove"), 2)
        if remote:
            self.assertLess(events.index("activity.remove"), events.index("member.remove"))
            self.assertLess(events.index("member.remove"), events.index("keyboard.unpair"))
        self.assertEqual(events[-2:], ["keyboard.unpair", "bed.close"])
        self.assertFalse(scenario.members)
        self.assertFalse(scenario.activities)
        self.assertIsNone(scenario.keyboard)

    def test_local_and_remote_topology_pair_on_main_then_require_all_input_proofs(self):
        for test, remote in self.cases:
            with self.subTest(test=test.__name__):
                scenario = KeyboardScenario()
                scenario.run(test)
                owner = "amity-dut-2" if remote else "amity-dut"
                self.assertEqual({component.dut.name for component in scenario.components}, {owner})
                self.assertEqual([component.direction for component in scenario.components], ["front", "back", "back", "back"])
                self.assertEqual([component.address.physical for component in scenario.components], [0, 0x1000, 0x1100, 0x1200])
                self.assertEqual([component.address.logical for component in scenario.components], [0, 5, 4, 3])
                self.assertEqual([row[1] for row in scenario.events if row[0] == "keyboard.pair"], ["amity-dut"])
                self.assertEqual([row[1] for row in scenario.events if row[0] == "scan.only"], [scenario.components])
                self.assertEqual(len([row for row in scenario.events if row[0] == "scan.on"]), 4)
                self.assertEqual([row[0] for row in scenario.events if row[0].startswith("input.")],
                                 ["input.observe", "input.tap", "input.overlap", "input.shared",
                                  "input.failed_press", "input.failed_repeat", "input.close",
                                  "input.observe", "input.disconnect", "input.close"])
                self.assertEqual([row[0] for row in scenario.events if row[0].startswith("transition.")],
                                 ["transition.observe", "transition.fallback", "transition.all_up", "transition.close"])
                creates = [index for index, row in enumerate(scenario.events) if row[0] == "activity.create"]
                first_start = next(index for index, row in enumerate(scenario.events) if row[0] == "activity.start")
                self.assertTrue(all(index < first_start for index in creates))
                power = [row for row in scenario.events if row[0] == "power.wait"]
                self.assertEqual([row[1] for row in power],
                                 ["start"] * 3 + ["standby"] + ["start"] * 3 + ["standby"] * 3)
                self.assertEqual({row[2] for row in power}, {owner})
                self.assertEqual([row[3] for row in scenario.events if row[0] == "activity.create"],
                                 [(scenario.components[0], scenario.components[1], scenario.components[2]),
                                  (None, None, scenario.components[3])])
                self.assertEqual([row[2] for row in scenario.events if row[0] == "activity.start"],
                                 ["Functional BLE keyboard"] * 2)
                self.assertEqual(len([row for row in scenario.events if row[0] == "route.wait"]), 2)
                self.assertTrue(all(component.power is PowerState.STANDBY for component in scenario.components))
                self.assert_cleanup(scenario, remote)

    def test_existing_bond_fails_before_member_scan_activity_or_input(self):
        for test, _ in self.cases:
            with self.subTest(test=test.__name__):
                scenario = KeyboardScenario(existing_bond=True)
                with self.assertRaisesRegex(RuntimeError, "Existing remote bond"):
                    scenario.run(test)
                self.assertEqual(scenario.events, [("bed.open",), ("keyboard.review", "amity-dut"), ("bed.close",)])

    def test_each_missing_physical_proof_fails_and_unwinds_owned_resources(self):
        for test, remote in self.cases:
            for kind in ("tap", "overlap", "shared", "failed_press", "failed_repeat",
                         "transition_all_up", "transition_fallback", "disconnect"):
                with self.subTest(test=test.__name__, kind=kind):
                    scenario = KeyboardScenario(failed_proof=kind)
                    with self.assertRaisesRegex(TimeoutError, "Missing fresh " + kind):
                        scenario.run(test)
                    self.assert_cleanup(scenario, remote)


if __name__ == "__main__":
    unittest.main()
