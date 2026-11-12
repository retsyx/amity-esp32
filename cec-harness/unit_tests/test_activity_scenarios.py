"""Activity announcements, repair and disconnected holds require wire evidence."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import (AvrComponent, Component, DUT, Key, PlaybackComponent,
                           PowerState, TvComponent)
from functional_tests import activity as scenarios
from unit_tests.test_consortium_scenarios import Scenario


class ActivityScenario(Scenario):
    def __init__(self, *, missing_repair_power=None, missing_repair_route=False,
                 repair_standby=False, disconnect_failure=False, missing_followup=False,
                 selected_standby=False, changed_active_selection=False,
                 directed_standby=False, changed_standby_selection=False,
                 standby_settling=False, standby_states=(), missing_display_input=False,
                 missing_repair_display_input=False, wrong_display_input=False,
                 wrong_repair_display_input=False):
        super().__init__()
        self.missing_repair_power = missing_repair_power
        self.missing_repair_route = missing_repair_route
        self.repair_standby = repair_standby
        self.disconnect_failure = disconnect_failure
        self.missing_followup = missing_followup
        self.selected_standby = selected_standby
        self.changed_active_selection = changed_active_selection
        self.directed_standby = directed_standby
        self.changed_standby_selection = changed_standby_selection
        self.standby_settling = standby_settling
        self.standby_states = tuple(standby_states)
        self.standby_reads = 0
        self.missing_display_input = missing_display_input
        self.missing_repair_display_input = missing_repair_display_input
        self.wrong_display_input = wrong_display_input
        self.wrong_repair_display_input = wrong_repair_display_input
        self.actions = []
        self.announcements = []
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
                    if (source.power != PowerState.STANDBY or audio.active_input != 0x1200
                            or display.active_input == display.tv_input << 12):
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
                # Even when the original announcement is missing/wrong, final
                # model state is correct: it cannot replace fresh wire proof.
                display._active_input = display.tv_input << 12
                display_input = display.tv_input << 12
                if fixture.missing_display_input or repair and fixture.missing_repair_display_input:
                    display_input = None
                elif fixture.wrong_display_input or repair and fixture.wrong_repair_display_input:
                    display_input = 0x2000 if display.tv_input == 1 else 0x1000
                power_commands = set(components)
                if repair and fixture.missing_repair_power is not None:
                    power_commands.discard(components[fixture.missing_repair_power])
                action = SimpleNamespace(kind="repair" if repair else "start", activity=self,
                                         owner=source.dut.name, power_commands=power_commands,
                                         route=not (repair and fixture.missing_repair_route),
                                         source=source, audio=audio, display=display,
                                         display_input=display_input)
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

    def wait_power(self, component, power, *, since, command=None, directed=False, broadcast=False):
        announcement = next((item for item in self.announcements if item.timestamp_ns == since), None)
        if announcement is not None:
            if (announcement.kind != "standby" or announcement.source is not component
                    or not broadcast or directed or command != "standby"):
                raise AssertionError("Standby theft requires this announcement's broadcast command")
            if announcement.destination != 15:
                raise TimeoutError("No fresh broadcast Standby command")
            self.assert_power(component, power)
            self.events.append(("announcement.standby", component, announcement))
            return component
        if since not in self.actions or component not in since.power_commands:
            raise AssertionError("Missing fresh physical power command")
        if power == PowerState.STANDBY and command != "standby":
            raise AssertionError("Standby requires a fresh Standby command")
        self.assert_power(component, power)
        self.events.append(("power.wait", since.kind, component))
        return component

    def activate(self, component):
        kind = "selected" if self.current is not None else "standby"
        announcement = SimpleNamespace(ok=True, timestamp_ns=time.monotonic_ns(),
                                       kind=kind, source=component,
                                       destination=component.address.logical if self.directed_standby else 15)
        self.announcements.append(announcement)
        self.events.append(("announcement.sent", kind, component))
        if kind == "selected":
            if self.observation is None:
                raise AssertionError("Observe selected equipment before its announcement")
            self.observation.saw_standby = self.selected_standby
            # A transient Standby must fail even after final model power recovers.
            component._power = PowerState.ON
            component.active = True
            if self.changed_active_selection:
                self.current = object()
        else:
            # Directed-only suppression can leave every model in Standby; the
            # scenario must still demand the original broadcast wire command.
            for recipient in self.components:
                recipient._power = PowerState.STANDBY
            component.active = False
        return announcement

    def state(self, main):
        self.events.append(("standby.state", main))
        if self.standby_states:
            selected, settling = self.standby_states[min(self.standby_reads, len(self.standby_states) - 1)]
            self.standby_reads += 1
            return {"current_activity": selected, "activity_settling": settling}
        return {"current_activity": 0 if self.changed_standby_selection else -1,
                "activity_settling": self.standby_settling}

    def route(self, avr, player, *, since):
        if (since not in self.actions or not since.route or since.audio is not avr
                or since.source is not player):
            raise AssertionError("Missing fresh physical routing command")
        self.events.append(("route.wait", since.kind, avr, player))
        return avr

    def display_route(self, display, *, since):
        if (since not in self.actions or since.display is not display
                or since.display_input != display.tv_input << 12):
            raise AssertionError("Missing fresh physical display input announcement")
        if display.active_input != display.tv_input << 12:
            raise AssertionError("The display did not select Amity's input")
        self.events.append(("display.wait", since.kind, display))
        return display

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

            def assert_preserved_after(self, announcement, *, within):
                if (announcement not in fixture.announcements or announcement.kind != "selected"
                        or announcement.source not in components or within != 0.250):
                    raise AssertionError("Selected-source observation needs its actual announcement and response window")
                if self.saw_standby:
                    raise AssertionError("Selected source received an unexpected physical Standby")
                self.checked = True
                fixture.events.append(("announcement.checked", announcement))

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
            patches.enter_context(patch.object(DUT, "get_state", lambda main: self.state(main)))
            patches.enter_context(patch.object(Component, "assert_power", lambda component, power: self.assert_power(component, power)))
            patches.enter_context(patch.object(Component, "wait_for_power", lambda component, power, **kw: self.wait_power(component, power, **kw)))
            patches.enter_context(patch.object(Component, "wait_for_input", lambda avr, player, **kw: self.route(avr, player, **kw)))
            patches.enter_context(patch.object(TvComponent, "wait_for_amity_input", lambda tv, **kw: self.display_route(tv, **kw)))
            patches.enter_context(patch.object(Component, "standby", lambda component: self.disturb(component)))
            patches.enter_context(patch.object(PlaybackComponent, "activate", lambda component: self.activate(component)))
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
        self.assertEqual([row[1] for row in fixture.events if row[0] == "display.wait"], ["start", "repair"])
        self.assert_cleanup(fixture)

    def test_selected_and_standby_announcements_are_checked_without_reselecting_activity(self):
        fixture = ActivityScenario()
        fixture.run()
        selected, standby = fixture.announcements
        self.assertEqual((selected.kind, standby.kind), ("selected", "standby"))
        self.assertIs(selected.source, standby.source)
        self.assertIn(("announcement.checked", selected), fixture.events)
        self.assertIn(("announcement.standby", standby.source, standby), fixture.events)
        self.assertEqual(len([row for row in fixture.events if row[0] == "activity.start"]), 2)
        self.assertEqual(len([row for row in fixture.events if row[0] == "standby.state"]), 1)
        self.assert_cleanup(fixture)

    def test_selected_source_transient_standby_fails_even_when_final_power_recovers(self):
        fixture = ActivityScenario(selected_standby=True)
        with self.assertRaisesRegex(AssertionError, "unexpected physical Standby"):
            fixture.run()
        self.assertTrue(all(component.power == PowerState.ON for component in fixture.components))
        self.assert_cleanup(fixture)

    def test_changed_selection_after_selected_announcement_fails(self):
        fixture = ActivityScenario(changed_active_selection=True)
        with self.assertRaisesRegex(AssertionError, "selected activity changed"):
            fixture.run()
        self.assert_cleanup(fixture)

    def test_directed_only_standby_response_fails_even_when_all_equipment_is_asleep(self):
        fixture = ActivityScenario(directed_standby=True)
        with self.assertRaisesRegex(TimeoutError, "broadcast Standby"):
            fixture.run()
        self.assertTrue(all(component.power == PowerState.STANDBY for component in fixture.components))
        self.assert_cleanup(fixture)

    def test_standby_suppression_waits_for_completion_publication_without_replaying_actions(self):
        fixture = ActivityScenario(standby_states=((-1, True), (-1, False)))
        fixture.run()
        self.assertEqual(fixture.standby_reads, 2)
        self.assertEqual(len(fixture.announcements), 2)
        self.assertEqual([action.kind for action in fixture.actions], ["start", "repair", "standby"])
        self.assertTrue(any(row[0] == "announcement.standby" for row in fixture.events))
        self.assert_cleanup(fixture)

    def test_changed_standby_selection_fails_even_during_pending_completion(self):
        for states in (((0, False),), ((-1, True), (0, True), (-1, False))):
            with self.subTest(states=states):
                fixture = ActivityScenario(standby_states=states)
                with self.assertRaises(AssertionError):
                    fixture.run()
                self.assertEqual(fixture.standby_reads, 1 if len(states) == 1 else 2)
                self.assertTrue(any(row[0] == "announcement.standby" for row in fixture.events))
                self.assert_cleanup(fixture)

    def test_standby_completion_that_never_publishes_still_times_out(self):
        fixture = ActivityScenario(standby_settling=True)
        with patch("amity_harness.dut.time.monotonic", side_effect=(0, 1_000_000_000)):
            with self.assertRaisesRegex(TimeoutError, "Amity UI state"):
                fixture.run()
        self.assertTrue(any(row[0] == "announcement.standby" for row in fixture.events))
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

    def test_missing_or_wrong_display_announcement_fails_even_when_final_input_matches(self):
        for option in ("missing_display_input", "wrong_display_input",
                       "missing_repair_display_input", "wrong_repair_display_input"):
            with self.subTest(option=option):
                fixture = ActivityScenario(**{option: True})
                with self.assertRaisesRegex(AssertionError, "Missing fresh physical display input"):
                    fixture.run()
                display, = (component for component in fixture.components if isinstance(component, TvComponent))
                self.assertEqual(display.active_input, display.tv_input << 12)
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
                display_proofs = [row for row in fixture.events if row[0] == "display.wait"]
                self.assertEqual(len(display_proofs), 1)
                self.assertEqual(display_proofs[0][2].dut.name, owner)
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


class SetupStepGapTests(unittest.TestCase):
    @staticmethod
    def row(stamp, *, gpio=23, source=0, direction="rx", transport="wire"):
        return {"direction": direction, "transport": transport, "gpio": gpio,
                "source": source, "wire_timestamp_ns": stamp}

    def test_largest_gap_uses_only_fresh_amity_wire_frames_inside_setup(self):
        records = [self.row(5), self.row(100), self.row(250, gpio=24), self.row(400),
                   self.row(300, source=4), self.row(320, transport="virtual"),
                   self.row(330, direction="tx"), self.row(340, gpio=99), self.row(900)]
        frames, gap = scenarios.setup_step_gap(records, {23: 0, 24: 0}, {23, 24}, since=10, through=400)
        self.assertEqual((frames, gap), (3, 150))

    def test_a_reply_wait_between_steps_is_the_reported_gap(self):
        records = [self.row(100), self.row(1_600_000_100), self.row(1_600_050_000)]
        frames, gap = scenarios.setup_step_gap(records, {23: 0}, {23}, since=0, through=2_000_000_000)
        self.assertEqual((frames, gap), (3, 1_600_000_000))
        self.assertEqual(scenarios.setup_step_gap([], {23: 0}, {23}, since=0, through=1), (0, 0))


if __name__ == "__main__":
    unittest.main()
