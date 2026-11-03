"""Address scenarios require UI and physical proof for each owner/address path."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import DUT, TvComponent, AvrComponent, PlaybackComponent
from functional_tests import default_physical_address
from unit_tests.test_consortium_scenarios import Scenario


class AddressScenario(Scenario):
    def __init__(self, *, missing_physical_proof=False, wrong_observation=False,
                 initial_default="4.0.0.0", initial_selected="4.0.0.0"):
        super().__init__()
        self.default, self.selected = initial_default, initial_selected
        self.settings = []
        self.scan_proof = None
        self.missing_physical_proof = missing_physical_proof
        self.wrong_observation = wrong_observation
        self.physical_checks = 0

    @contextmanager
    def bed(self, *resources, members):
        with super().bed(*resources, members=members) as bed:
            yield bed
            if self.settings:
                raise AssertionError("Restore settings before releasing their owner")

    @property
    def target(self):
        return self.components[0].dut

    @property
    def detected(self):
        return "1.0.0.0" if len(self.components) == 3 else None

    def add(self, main, candidate):
        member = super().add(main, candidate)
        member.front_physical_address = lambda tentacle, address: self.address(candidate, tentacle, address)
        return member

    def scan(self, main):
        self.events.append(("scan", main))
        self.scan_proof = super().scan(main)
        return self.scan_proof

    def address(self, owner, tentacle, address):
        if owner is not self.target or tentacle != 0 or self.scan_proof is None:
            raise AssertionError("Save the scanned owner's configured adapter")
        if owner in self.member_duts and owner not in self.members:
            raise AssertionError("Remote settings require a test-owned enrolled member")
        fixture = self

        class Address:
            def __init__(self):
                self.previous = fixture.default
                self.closed = False
                self.changed = self.previous != address
                if self.changed:
                    fixture.default = address
                    fixture.selected = fixture.detected or address
                fixture.settings.append(self)
                fixture.events.append(("settings.apply", owner.name, address))

            def __enter__(self):
                return self

            def read(self):
                fixture.events.append(("settings.read", owner.name, fixture.selected))
                return {"default_front_physical_address": fixture.default,
                        "observed_front_physical_address": fixture.detected,
                        "effective_front_physical_address": fixture.selected}

            def assert_address(self, *, detected, selected):
                observed = "9.0.0.0" if fixture.wrong_observation else fixture.detected
                if fixture.default != address or observed != detected or fixture.selected != selected:
                    raise AssertionError("UI address fields do not match the expected address source")
                fixture.events.append(("settings.proof", owner.name, address, detected, selected))

            def close(self):
                if self.closed:
                    return
                if fixture.settings[-1] is not self:
                    raise AssertionError("Restore nested defaults in reverse order")
                fixture.settings.pop()
                if self.changed:
                    fixture.default = self.previous
                    fixture.selected = fixture.detected or self.previous
                fixture.events.append(("settings.restore", owner.name, self.previous))
                self.closed = True

            def __exit__(self, *args):
                self.close()

        return Address()

    @contextmanager
    def evidence(self, main, tv, scan):
        if (main is not self.duts[0] or tv is not self.components[0]
                or tv.direction != "front" or scan is not self.scan_proof):
            raise AssertionError("Physical proof must retain the original scan and exact owner/wire")
        fixture = self
        fixture.events.append(("wire.open",))

        class Evidence:
            def assert_selected(self, selected, *, changed):
                fixture.physical_checks += 1
                if fixture.missing_physical_proof and fixture.physical_checks == 2:
                    raise TimeoutError("No fresh physical Report Physical Address")
                expected_changed = fixture.physical_checks > 1 and fixture.detected is None
                if fixture.selected != selected or changed != expected_changed:
                    raise AssertionError("Require the selected physical identity and changed-address announcement")
                fixture.events.append(("wire.proof", tv.dut.name, selected, changed))

        try:
            yield Evidence()
        finally:
            fixture.events.append(("wire.close",))

    def run(self, test):
        with ExitStack() as patches:
            patches.enter_context(patch.object(default_physical_address, "TestBed", self.bed))
            patches.enter_context(patch.object(default_physical_address, "AdapterAddressEvidence", self.evidence))
            patches.enter_context(patch.object(DUT, "add_amity", lambda main, candidate: self.add(main, candidate)))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda main: self.scan(main)))
            patches.enter_context(patch.object(DUT, "front_physical_address",
                                               lambda owner, tentacle, address: self.address(owner, tentacle, address),
                                               create=True))
            test()


class DefaultPhysicalAddressScenarioTests(unittest.TestCase):
    cases = (
        (default_physical_address.test_default_physical_address_local, False, False),
        (default_physical_address.test_default_physical_address_local_scanned, False, True),
        (default_physical_address.test_default_physical_address_remote, True, False),
        (default_physical_address.test_default_physical_address_remote_scanned, True, True),
    )

    def assert_restored(self, scenario, remote, *, expected_default="4.0.0.0"):
        self.assertEqual(scenario.default, expected_default)
        self.assertFalse(scenario.settings)
        self.assertFalse(scenario.members)
        self.assertEqual(scenario.events[-1], ("bed.close",))
        restored = [index for index, event in enumerate(scenario.events) if event[0] == "settings.restore"]
        if remote:
            removed = next(index for index, event in enumerate(scenario.events) if event[0] == "member.remove")
            self.assertLess(max(restored), removed)

    def test_all_four_paths_prove_ui_and_physical_identity_then_restore(self):
        for test, remote, scanned in self.cases:
            with self.subTest(test=test.__name__):
                scenario = AddressScenario()
                scenario.run(test)
                target = "amity-dut-2" if remote else "amity-dut"
                self.assertEqual([dut.name for dut in scenario.member_duts], [target] if remote else [])
                self.assertEqual({component.dut.name for component in scenario.components}, {target})
                self.assertIsInstance(scenario.components[0], TvComponent)
                self.assertEqual(scenario.components[0].direction, "front")
                self.assertEqual(len(scenario.components), 3 if scanned else 1)
                if scanned:
                    self.assertIsInstance(scenario.components[1], AvrComponent)
                    self.assertIsInstance(scenario.components[2], PlaybackComponent)
                    self.assertEqual([component.direction for component in scenario.components[1:]], ["back", "back"])
                    self.assertEqual([component.address.physical for component in scenario.components[1:]], [0x1000, 0x1100])
                self.assertEqual([event[1] for event in scenario.events if event[0] == "scan.only"], [scenario.components])
                self.assertEqual(len([event for event in scenario.events if event[0] == "scan.on"]), len(scenario.components))
                self.assertEqual([event[2] for event in scenario.events if event[0] == "settings.apply"],
                                 ["2.0.0.0", "1.0.0.0", "2.0.0.0"])
                expected = ["1.0.0.0"] * 3 if scanned else ["1.0.0.0", "2.0.0.0", "1.0.0.0"]
                priming = "1.0.0.0" if scanned else "2.0.0.0"
                self.assertEqual([event[2:] for event in scenario.events if event[0] == "wire.proof"],
                                 [(priming, False)] + [(address, not scanned) for address in expected])
                self.assertEqual([event[3:] for event in scenario.events if event[0] == "settings.proof"],
                                 [("1.0.0.0" if scanned else None, address) for address in expected])
                primed = next(index for index, event in enumerate(scenario.events) if event[0] == "wire.proof")
                baseline = scenario.events.index(("settings.apply", target, "1.0.0.0"))
                self.assertLess(primed, baseline)
                self.assert_restored(scenario, remote)

    def test_noop_priming_queries_current_selection_before_the_first_real_save(self):
        for test, remote, _ in self.cases:
            with self.subTest(test=test.__name__):
                scenario = AddressScenario(initial_default="2.0.0.0", initial_selected="4.0.0.0")
                scenario.run(test)
                proofs = [event for event in scenario.events if event[0] == "wire.proof"]
                self.assertEqual(proofs[0][2:], ("4.0.0.0", False))
                primed = scenario.events.index(proofs[0])
                baseline = next(index for index, event in enumerate(scenario.events)
                                if event[0] == "settings.apply" and event[2] == "1.0.0.0")
                self.assertLess(primed, baseline)
                self.assert_restored(scenario, remote, expected_default="2.0.0.0")

    def test_missing_physical_proof_fails_and_restores_before_member_removal(self):
        for test, remote, _ in self.cases:
            with self.subTest(test=test.__name__):
                scenario = AddressScenario(missing_physical_proof=True)
                with self.assertRaisesRegex(TimeoutError, "fresh physical"):
                    scenario.run(test)
                self.assert_restored(scenario, remote)

    def test_unexpected_detected_address_cannot_pass_with_correct_selected_address(self):
        for test, remote, _ in self.cases:
            with self.subTest(test=test.__name__):
                scenario = AddressScenario(wrong_observation=True)
                with self.assertRaisesRegex(AssertionError, "expected address source"):
                    scenario.run(test)
                self.assertEqual(scenario.physical_checks, 1)
                self.assert_restored(scenario, remote)


if __name__ == "__main__":
    unittest.main()
