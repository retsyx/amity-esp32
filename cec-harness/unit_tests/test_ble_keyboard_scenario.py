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
    def activity(self, *args, **kwargs):
        with super().activity(*args, **kwargs) as activity:
            try:
                yield activity
            finally:
                # Model the real fixture's exception cleanup; a happy-path
                # scenario must still explicitly assert its fresh Standby.
                if self.current is activity:
                    activity.standby()

    @contextmanager
    def evidence(self, main, scan, *components):
        if main is not self.duts[0] or scan is not self.scan_proof or components != self.components:
            raise AssertionError("Input evidence needs the exact scanned owner and components")
        self.events.append(("input.observe",))
        fixture = self

        class Evidence:
            def check(self, kind, keyboard, *recipients):
                if keyboard is not fixture.keyboard or fixture.current is None:
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

            def assert_disconnect_cleanup(self, keyboard, avr):
                if avr is not fixture.components[1]:
                    raise AssertionError("Disconnect cleanup must release the held AVR key")
                self.check("disconnect", keyboard, avr)
                keyboard.connected = False

        try:
            yield Evidence()
        finally:
            self.events.append(("input.close",))

    def run(self, test):
        with ExitStack() as patches:
            replacements = (
                (ble_keyboard, "TestBed", self.bed),
                (ble_keyboard, "BLEInputEvidence", self.evidence),
                (DUT, "pair_ble_keyboard", lambda main: self.pair(main)),
                (DUT, "add_amity", lambda main, candidate: self.add(main, candidate)),
                (DUT, "scan_hdmi", lambda main: self.scan(main)),
                (DUT, "create_activity", lambda main, name, **kwargs: self.activity(main, name, **kwargs)),
                (Component, "wait_for_power", lambda component, power, **kwargs: self.wait_power(component, power, **kwargs)),
                (Component, "wait_for_input", lambda avr, player, **kwargs: self.route(avr, player, **kwargs)),
            )
            for target, name, replacement in replacements:
                patches.enter_context(patch.object(target, name, replacement))
            test()


class BLEKeyboardScenarioTests(unittest.TestCase):
    cases = ((ble_keyboard.test_ble_keyboard_local, False), (ble_keyboard.test_ble_keyboard_remote, True))

    def assert_cleanup(self, scenario, remote):
        events = [event[0] for event in scenario.events]
        self.assertLess(events.index("activity.remove"), events.index("keyboard.unpair"))
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
                self.assertEqual([component.direction for component in scenario.components], ["front", "back", "back"])
                self.assertEqual([component.address.physical for component in scenario.components], [0, 0x1000, 0x1100])
                self.assertEqual([row[1] for row in scenario.events if row[0] == "keyboard.pair"], ["amity-dut"])
                self.assertEqual([row[1] for row in scenario.events if row[0] == "scan.only"], [scenario.components])
                self.assertEqual(len([row for row in scenario.events if row[0] == "scan.on"]), 3)
                self.assertEqual([row[0] for row in scenario.events if row[0].startswith("input.")],
                                 ["input.observe", "input.tap", "input.overlap", "input.disconnect", "input.close"])
                power = [row for row in scenario.events if row[0] == "power.wait"]
                self.assertEqual([row[1] for row in power], ["start"] * 3 + ["standby"] * 3)
                self.assertEqual({row[2] for row in power}, {owner})
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
            for kind in ("tap", "overlap", "disconnect"):
                with self.subTest(test=test.__name__, kind=kind):
                    scenario = KeyboardScenario(failed_proof=kind)
                    with self.assertRaisesRegex(TimeoutError, "Missing fresh " + kind):
                        scenario.run(test)
                    self.assert_cleanup(scenario, remote)


if __name__ == "__main__":
    unittest.main()
