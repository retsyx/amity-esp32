"""Scenario coverage; physical-frame qualification belongs to shared helpers."""

from contextlib import ExitStack, contextmanager
from importlib import import_module
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrAttachment, Component, DUT, PlaybackComponent, PowerState
from amity_harness.config import current_config
from unit_tests.test_consortium_scenarios import Scenario

source_theft = import_module("functional_tests.source-theft")


class IsolatedScenario(Scenario):
    def __init__(self, *, disconnected=False, missing_positive=False, leaked=False):
        super().__init__()
        self.disconnected, self.missing_positive, self.leaked = disconnected, missing_positive, leaked
        self.address_applied = False
        self.announcements = []

    def add(self, main, candidate):
        member = super().add(main, candidate)

        @contextmanager
        def address(tentacle, value):
            if (tentacle, value) != (2, "1.2.0.0") or self.current is not None:
                raise AssertionError("Review the AVR input's exact address before scan/activity")
            self.address_applied = True
            self.events.append(("settings.apply", tentacle, value))
            try:
                yield
            finally:
                if self.activities:
                    raise AssertionError("Close activities before restoring adapter settings")
                self.address_applied = False
                self.events.append(("settings.restore",))

        member.front_physical_address = address
        return member

    def scan(self, main):
        if not self.address_applied:
            raise AssertionError("Apply the UI address before discovery")
        self.events.append(("scan",))
        scan = super().scan(main)
        original = scan.assert_on

        def location(component, endpoint):
            if component.endpoint != endpoint:
                raise AssertionError("Discovery must prove the exact owner, tentacle and CEC side")
            self.events.append(("scan.location", component, endpoint))
            return original(component, endpoint)

        scan.assert_on = location
        return scan

    def activity(self, main, name, *, display, audio, source):
        activity = super().activity(main, name, display=display, audio=audio, source=source)
        activity.source, activity.audio = source, audio
        return activity

    def activate(self, thief):
        if self.current is None or self.current.source is thief:
            raise AssertionError("A theft attempt needs the other source's activity")
        thief._power, thief.active = PowerState.STANDBY, False
        announcement = SimpleNamespace(ok=True, kind="theft", thief=thief)
        self.announcements.append(announcement)
        self.events.append(("theft", thief))
        return announcement

    @contextmanager
    def observe(self, avr, thief, *, upstream, since, duration=None):
        if (not isinstance(upstream, AvrAttachment) or upstream._avr_device is not avr
                or upstream.endpoint != thief.dut.tentacle(2).front
                or len({avr.gpio, upstream.gpio, thief.gpio}) != 3 or since.kind != "start"):
            raise AssertionError("Observe both real AVR paths and the protected activity boundary")
        budget = current_config().cec_timeout if duration is None else duration
        self.events.append(("watch.open", upstream, budget))

        def suppressed(announcement):
            if announcement not in self.announcements or announcement.thief is not thief:
                raise AssertionError("The absence proof needs this exact physical announcement")
            if self.leaked:
                raise AssertionError("The upstream wire observed a leaked source announcement")
            self.events.append(("suppression", announcement, budget))

        try:
            yield SimpleNamespace(assert_suppressed=suppressed)
        finally:
            self.events.append(("watch.close",))

    def standby_after(self, thief, announcement, *, within):
        self.events.append(("remediation", announcement, within))
        return super().standby_after(thief, announcement, within=within)

    def active_source(self, upstream, thief, *, since):
        # Scenario.start has already made the AVR's local route look correct.
        # Only the separate positive physical command proof can accept it.
        self.assert_route_state = upstream.active_input == thief.address.physical
        self.events.append(("positive", upstream, thief))
        if self.disconnected or self.missing_positive:
            raise TimeoutError("No fresh upstream physical Active Source")
        return upstream

    def run(self):
        with ExitStack() as patches:
            replacements = (
                (source_theft, "TestBed", self.bed),
                (DUT, "add_amity", lambda main, candidate: self.add(main, candidate)),
                (DUT, "scan_hdmi", lambda main: self.scan(main)),
                (DUT, "create_activity", lambda main, name, **kw: self.activity(main, name, **kw)),
                (Component, "assert_power", lambda component, power: self.assert_power(component, power)),
                (Component, "wait_for_power", lambda component, power, **kw: self.wait_power(component, power, **kw)),
                (Component, "wait_for_standby_after", lambda component, announcement, **kw: self.standby_after(component, announcement, **kw)),
                (Component, "wait_for_input", lambda avr, player, **kw: self.route(avr, player, **kw)),
                (Component, "wait_for_active_source", lambda upstream, thief, **kw: self.active_source(upstream, thief, **kw)),
                (Component, "observe_source_isolation", lambda avr, thief, **kw: self.observe(avr, thief, **kw)),
                (PlaybackComponent, "activate", lambda thief: self.activate(thief)),
            )
            for target, name, replacement in replacements:
                patches.enter_context(patch.object(target, name, replacement))
            source_theft.test_source_theft_isolated()


class IsolatedSourceTheftScenarioTests(unittest.TestCase):
    def assert_cleanup(self, scenario):
        cleanup = [row[0] for row in scenario.events
                   if row[0] in {"activity.remove", "settings.restore", "member.remove", "bed.close"}]
        self.assertEqual(cleanup, ["activity.remove", "activity.remove", "settings.restore",
                                   "member.remove", "bed.close"])
        self.assertFalse(scenario.address_applied)

    def test_exact_topology_address_order_and_three_physical_theft_checks(self):
        scenario = IsolatedScenario()
        scenario.run()
        main, secondary = scenario.duts
        self.assertEqual((main.name, secondary.name), ("amity-dut", "amity-dut-4"))
        tv, avr, player, upstream, thief = scenario.components
        self.assertIs(upstream._avr_device, avr)
        self.assertEqual(len({id(getattr(c, "_avr_device", c)) for c in scenario.components}), 4)
        self.assertEqual((tv.endpoint, avr.endpoint, upstream.endpoint, thief.endpoint),
                         (secondary.tentacle(1).front, secondary.tentacle(1).back,
                          secondary.tentacle(2).front, secondary.tentacle(2).back))
        self.assertIs(player.parent, avr)
        self.assertEqual((player.port, player.address.physical, thief.address.physical), (1, 0x1100, 0x1200))
        for kind in ("scan.contains", "scan.only"):
            self.assertEqual([row[1] for row in scenario.events if row[0] == kind], [scenario.components])
        self.assertEqual(len([row for row in scenario.events if row[0] == "scan.on"]), 5)
        self.assertEqual({row[1:] for row in scenario.events if row[0] == "scan.location"},
                         {(component, component.endpoint) for component in scenario.components})
        kinds = [row[0] for row in scenario.events]
        self.assertLess(kinds.index("settings.apply"), kinds.index("scan"))
        self.assertLess(kinds.index("scan"), kinds.index("activity.create"))
        self.assertEqual(len(scenario.announcements), 3)
        for announcement in scenario.announcements:
            self.assertIn(("remediation", announcement, 0.250), scenario.events)
            self.assertIn(("suppression", announcement, 5), scenario.events)
        self.assertEqual(len([row for row in scenario.events if row[0] == "positive"]), 1)
        self.assert_cleanup(scenario)

    def test_disconnected_upstream_or_missing_positive_command_cannot_pass_on_route_state(self):
        for fault in ("disconnected", "missing_positive"):
            with self.subTest(fault=fault):
                scenario = IsolatedScenario(**{fault: True})
                with self.assertRaisesRegex(TimeoutError, "fresh upstream physical Active Source"):
                    scenario.run()
                self.assertTrue(scenario.assert_route_state)
                self.assert_cleanup(scenario)

    def test_upstream_leak_fails_even_when_the_avr_retains_the_correct_route(self):
        scenario = IsolatedScenario(leaked=True)
        with self.assertRaisesRegex(AssertionError, "leaked source announcement"):
            scenario.run()
        _, avr, player, _, _ = scenario.components
        self.assertEqual(avr.active_input, player.address.physical)
        self.assert_cleanup(scenario)


if __name__ == "__main__":
    unittest.main()
