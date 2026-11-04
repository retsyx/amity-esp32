"""Nondefault activity topologies retain explicit audio intent and wire evidence."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecAddress, Component, DUT, Key, PlaybackComponent, PowerState, TvComponent
from functional_tests import activity as scenarios
from unit_tests.test_consortium_scenarios import Scenario


class NondefaultScenario(Scenario):
    def __init__(self, *, receiver, remote=False, fault=None):
        super().__init__()
        self.receiver, self.remote, self.fault = receiver, remote, fault
        self.actions, self.settings = [], []
        self.scan_count, self.selected_address = 0, "1.0.0.0"

    @contextmanager
    def bed(self, *resources, members=()):
        with super().bed(*resources, members=members):
            self.tv, = (component for component in self.components if isinstance(component, TvComponent))
            self.player, = (component for component in self.components if isinstance(component, PlaybackComponent))
            self.avr = next((component for component in self.components if isinstance(component, AvrComponent)), None)
            if self.fault == "topology":
                self.player.address = CecAddress(self.player.address.logical, 0x1000)
            yield self
            if self.settings:
                raise AssertionError("Restore settings before releasing the fixture")

    def scan(self, dut):
        if dut.name != "amity-dut" or tuple(self.members) != self.member_duts:
            raise AssertionError("Scan through the original main after verifying the required members")
        self.scan_count += 1
        self.events.append(("scan", self.scan_count))
        if self.scan_count > 1 and self.fault in {"cleanup scan", "primary and cleanup"}:
            raise RuntimeError("Cleanup scan failed")
        present = {component for component in self.components if component.power != PowerState.OFF}
        fixture = self

        class Scan:
            def assert_contains_only(self, *components):
                if set(components) != present:
                    raise AssertionError("Enumeration does not match the responding fixture")
                fixture.events.append(("scan.only", tuple(components)))

            def assert_on(self, component, endpoint):
                if component not in present or component.endpoint != endpoint:
                    raise AssertionError("Discovery must prove the exact parsed owner/adapter/side")
                fixture.events.append(("scan.on", component))

        return Scan()

    def add(self, main, candidate):
        member = super().add(main, candidate)
        member.front_physical_address = lambda tentacle, address: self.address(candidate, tentacle, address)
        return member

    def local_address(self, dut, tentacle, address):
        if self.remote or dut.name != "amity-dut":
            raise AssertionError("Use the verified member's main-mediated settings operation")
        return self.address(dut, tentacle, address)

    def address(self, dut, tentacle, address):
        if (not self.scan_count or tentacle != 0 or address != "3.0.0.0" or dut is not self.tv.dut
                or self.remote and dut not in self.members):
            raise AssertionError("Select the nondefault address through the scanned adapter settings")
        fixture = self

        class Address:
            def __enter__(self):
                fixture.settings.append(self)
                fixture.selected_address = address
                fixture.events.append(("settings.apply",))
                return self

            def assert_address(self, *, detected, selected):
                if detected != "3.0.0.0" or selected != fixture.selected_address:
                    raise AssertionError("The selected UI address differs from the scanned topology")

            def __exit__(self, exc_type, exc, traceback):
                fixture.settings.remove(self)
                if fixture.activities:
                    raise AssertionError("Close the activity before restoring adapter settings")
                if not all(component.power == PowerState.OFF for component in fixture.components if component is not fixture.tv):
                    raise AssertionError("Disconnect back responders before restoring adapter settings")
                fixture.selected_address = "1.0.0.0" if fixture.scan_count > 1 else "3.0.0.0"
                fixture.events.append(("settings.restore",))

        return Address()

    def activity(self, dut, name, *, display, source, audio):
        if dut.name != "amity-dut":
            raise AssertionError("Only the original main saves activities")
        if display is not self.tv or source is not self.player or audio is not self.avr:
            raise AssertionError("Retain the requested receiver or explicit absence without substituting the TV")
        if not self.settings:
            raise AssertionError("The adapter settings scope must cover the entire activity")
        self.saved_audio = audio
        fixture = self

        class Activity:
            activity_id = "a" * 32

            def __enter__(self):
                fixture.activities.append(self)
                fixture.events.append(("activity.create",))
                return self

            def start(self):
                repair = fixture.current is self
                if repair and (source.power != PowerState.STANDBY or display.active_input != 0x1000
                               or audio is not None and audio.active_input != 0x3100):
                    raise AssertionError("Repair must disturb the selected source and relevant inputs")
                commands = set(fixture.components)
                for component in commands:
                    component._power = PowerState.ON
                source.active = True
                display._active_input = 0x3000
                if audio is not None:
                    audio._active_input = 0x3200
                action = SimpleNamespace(kind="repair" if repair else "start", commands=commands,
                                         front=0x3000, back=0x3200)
                if fixture.fault == "front input":
                    action.front = 0x1000
                if fixture.fault == "back input":
                    action.back = 0x3100
                if repair and fixture.fault == "repair power":
                    action.commands.remove(source)
                fixture.actions.append(action)
                fixture.current = self
                fixture.events.append(("activity.start", action.kind))
                return action

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The saved activity is no longer current")

            def standby(self):
                action = SimpleNamespace(kind="standby", commands=set(fixture.components))
                for component in fixture.components:
                    component._power = PowerState.STANDBY
                source.active = False
                fixture.actions.append(action)
                fixture.current = None
                fixture.events.append(("activity.standby",))
                return action

            def __exit__(self, *args):
                fixture.current = None
                fixture.activities.remove(self)
                fixture.events.append(("activity.remove",))

        return Activity()

    def configuration(self, dut):
        if dut.name != "amity-dut":
            raise AssertionError("Only the original main reads saved activity configuration")
        return {"activities": [{"activity_id": self.activities[0].activity_id,
                                 "audio": "component:TV" if self.fault == "saved TV audio"
                                 else "component:AVR" if self.receiver else ""}]}

    def wait_power(self, component, power, *, since, command=None):
        if self.fault == "primary and cleanup":
            raise AssertionError("Primary power evidence failed")
        if since not in self.actions or component not in since.commands:
            raise AssertionError("Missing fresh physical power command")
        if power == PowerState.STANDBY and command != "standby":
            raise AssertionError("Require the physical Standby command")
        self.assert_power(component, power)
        self.events.append(("power.wait", since.kind, component))
        return component

    def display_input(self, tv, *, since):
        if since not in self.actions or since.front != 0x3000 or tv.active_input != 0x3000:
            raise AssertionError("Missing fresh front Active Source at input 3")
        self.events.append(("front.input", since.kind))
        return tv

    def receiver_input(self, avr, source, *, since):
        if since not in self.actions or since.back != source.address.physical or avr.active_input != 0x3200:
            raise AssertionError("Missing fresh back Set Stream Path at input 3.2")
        self.events.append(("back.input", since.kind))
        return avr

    def key(self, dut, key):
        if dut.name != "amity-dut":
            raise AssertionError("Send keys through the original main")
        fixture = self
        volume = key in {Key.VOLUME_UP, Key.VOLUME_DOWN, Key.MUTE}
        target = (self.avr if self.receiver else self.tv) if volume else self.player

        class Action:
            def assert_received_by(self, recipient):
                if (recipient is not target or fixture.fault == "navigation target" and not volume
                        or fixture.fault == "volume target" and volume):
                    raise AssertionError("The physical key reached the wrong recipient")
                if fixture.fault != "audio effect" or not volume:
                    target.on_key(key)
                fixture.events.append(("key.received", key, target))

        return Action()

    def off(self, component):
        component._power = PowerState.OFF
        self.events.append(("fixture.off", component))

    def disturb(self, component):
        component._power = PowerState.STANDBY
        component.active = False

    def run(self):
        with ExitStack() as patches:
            for target, name, replacement in (
                (scenarios, "TestBed", self.bed),
                (DUT, "add_amity", self.add),
                (DUT, "scan_hdmi", self.scan),
                (DUT, "front_physical_address", self.local_address),
                (DUT, "create_activity", self.activity),
                (DUT, "get_configuration", self.configuration),
                (DUT, "press_key", self.key),
                (Component, "wait_for_power", self.wait_power),
                (TvComponent, "wait_for_amity_input", self.display_input),
                (Component, "wait_for_input", self.receiver_input),
                (Component, "power_off", self.off),
                (Component, "standby", self.disturb),
            ):
                patches.enter_context(patch.object(target, name, lambda *args, method=replacement, **kw: method(*args, **kw)))
            scenario = (scenarios.test_activity_without_receiver_secondary if self.remote else
                        scenarios.test_activity_nondefault_inputs if self.receiver else scenarios.test_activity_without_receiver)
            scenario()


class NondefaultScenarioTests(unittest.TestCase):
    def assert_cleanup(self, fixture):
        self.assertFalse(fixture.activities)
        self.assertFalse(fixture.settings)
        self.assertIsNone(fixture.current)
        self.assertEqual(fixture.events[-1], ("bed.close",))

    def test_receiver_absence_and_override_use_derived_nondefault_topology_and_fresh_targets(self):
        for receiver, remote in ((False, False), (False, True), (True, False)):
            with self.subTest(receiver=receiver, remote=remote):
                fixture = NondefaultScenario(receiver=receiver, remote=remote)
                fixture.run()
                self.assertEqual(len(fixture.components), 3 if receiver else 2)
                self.assertEqual(fixture.tv.tv_input, 3)
                self.assertEqual(fixture.tv.dut.name, "amity-dut-2" if remote else "amity-dut")
                self.assertEqual(fixture.player.address.physical, 0x3200 if receiver else 0x3000)
                self.assertIs(fixture.saved_audio, fixture.avr)
                if receiver:
                    self.assertEqual(fixture.avr.address.physical, 0x3000)
                    self.assertEqual(fixture.player.port, 2)
                else:
                    self.assertIsNone(fixture.player.parent)
                target = fixture.avr if receiver else fixture.tv
                keys = [row[1:] for row in fixture.events if row[0] == "key.received"]
                self.assertEqual(keys, [(Key.RIGHT, fixture.player), (Key.SELECT, fixture.player),
                                        (Key.VOLUME_UP, target), (Key.VOLUME_DOWN, target), (Key.MUTE, target)] * 2)
                self.assertEqual([row[1] for row in fixture.events if row[0] == "front.input"], ["start", "repair"])
                self.assertEqual([row[1] for row in fixture.events if row[0] == "back.input"],
                                 ["start", "repair"] if receiver else [])
                self.assertEqual(fixture.selected_address, "1.0.0.0")
                kinds = [row[0] for row in fixture.events]
                self.assertLess(kinds.index("activity.remove"), kinds.index("fixture.off"))
                self.assertLess(kinds.index("fixture.off"), kinds.index("settings.restore"))
                if remote:
                    self.assertLess(kinds.index("settings.restore"), kinds.index("member.remove"))
                    self.assertEqual([row[1] for row in fixture.events if row[0] == "member.connected"],
                                     ["amity-dut-2", "amity-dut-2"])
                self.assertEqual([row[1] for row in fixture.events if row[0] == "scan.only"][-1], (fixture.tv,))
                self.assert_cleanup(fixture)

    def test_missing_route_power_wrong_receiver_and_effects_cannot_pass_from_final_state(self):
        for receiver, remote in ((False, False), (False, True), (True, False)):
            faults = ["topology", "front input", "repair power", "navigation target", "volume target", "audio effect"]
            faults.append("back input" if receiver else "saved TV audio")
            for fault in faults:
                with self.subTest(receiver=receiver, remote=remote, fault=fault):
                    fixture = NondefaultScenario(receiver=receiver, remote=remote, fault=fault)
                    with self.assertRaises(AssertionError):
                        fixture.run()
                    self.assert_cleanup(fixture)

    def test_teardown_failure_is_reported_and_does_not_mask_a_primary_failure(self):
        for fault, error, message in (("cleanup scan", RuntimeError, "Cleanup scan failed"),
                                      ("primary and cleanup", AssertionError, "Primary power evidence failed")):
            with self.subTest(fault=fault):
                fixture = NondefaultScenario(receiver=False, fault=fault)
                with self.assertRaisesRegex(error, message):
                    fixture.run()
                self.assert_cleanup(fixture)


if __name__ == "__main__":
    unittest.main()
