"""The cascade scenario must reach Remove before closing its dependent rows."""

from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, DUT, PowerState
from amity_harness.components import Component
from functional_tests import removal_cascade as scenario


class CascadeScenarioTests(unittest.TestCase):
    def execute(self, *, fail=None):
        journal, activities = [], []
        member = SimpleNamespace(removed=False)

        class Activity:
            def __init__(self, name, selections):
                self.name, self.selections, self.closed = name, selections, False
                activities.append(self)
                journal.append(("create", name))

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.closed = True
                journal.append(("close", self.name))

            def start(self):
                if not member.removed:
                    raise AssertionError("Keeper was started before the cascade completed")
                journal.append(("start", self.name))
                return SimpleNamespace(kind="start", activity=self)

            def assert_active(self):
                journal.append(("active", self.name))

            def standby(self):
                journal.append(("standby", self.name))
                return SimpleNamespace(kind="standby", activity=self)

        class Review:
            def __init__(self, affected):
                self.affected = affected
                journal.append(("review", tuple(activity.name for activity in affected)))

            def assert_stale_rejected(self):
                if len(activities) != 6 or any(activity.closed for activity in self.affected):
                    raise AssertionError("Stale review did not retain its affected rows and new keeper")
                journal.append(("stale_rejected",))
                if fail == "stale":
                    raise AssertionError("injected stale-confirmation failure")

            def confirm(self):
                if any(activity.closed for activity in self.affected):
                    raise AssertionError("Dependent rows were cleaned before Remove")
                journal.append(("confirm", tuple(activity.name for activity in self.affected)))
                if fail == "confirm":
                    raise AssertionError("injected cascade failure")
                member.removed = True

        class Member:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                if any(not activity.closed for activity in activities):
                    raise AssertionError("Member cleanup overtook activity cleanup")
                journal.append(("member_close",))

            def assert_connected(self):
                journal.append(("connected",))

            def review_removal(self, *affected):
                return Review(affected)

        class Bed:
            def __init__(self, main, secondary, *components, members):
                if members != (secondary,) or len(components) != 6:
                    raise AssertionError("The scenario lost its named headless member or equipment")

            def __enter__(self):
                return self

            def __exit__(self, *args):
                journal.append(("bed_close",))

        class Scan:
            def assert_contains(self, *components):
                journal.append(("contains", len(components)))

            def assert_contains_only(self, *components):
                journal.append(("only", len(components)))

            def assert_on(self, component, endpoint):
                if component.endpoint != endpoint:
                    raise AssertionError("The scan used the wrong physical endpoint")
                journal.append(("location", component.name))

        def power(component, state, *, since, command=None):
            if component.dut.name != "amity-dut" or not member.removed:
                raise AssertionError("Post-removal power proof must use the retained local activity")
            expected = PowerState.ON if since.kind == "start" else PowerState.STANDBY
            if state != expected or command != (None if since.kind == "start" else "standby"):
                raise AssertionError("Power proof lost its fresh action or Standby command")
            journal.append(("power", component.name, state))

        def route(component, player, *, since):
            if component.dut.name != "amity-dut" or player.dut is not component.dut or since.kind != "start":
                raise AssertionError("Post-removal routing proof must stay on the retained owner")
            journal.append(("route", component.name, player.name))

        with ExitStack() as patches:
            patches.enter_context(patch.object(scenario, "TestBed", Bed))
            patches.enter_context(patch.object(DUT, "add_amity", lambda *args: Member()))
            patches.enter_context(patch.object(DUT, "scan_hdmi", lambda *args: Scan()))
            patches.enter_context(patch.object(DUT, "create_activity",
                                              lambda _, name, **selections: Activity(name, selections)))
            patches.enter_context(patch.object(Component, "wait_for_power", power))
            patches.enter_context(patch.object(AvrComponent, "wait_for_input", route))
            if fail:
                with self.assertRaisesRegex(AssertionError, "injected"):
                    scenario.test_removal_cascade()
            else:
                scenario.test_removal_cascade()
        return journal, activities, member

    def test_remove_keeps_all_four_role_rows_live_then_proves_unrelated_activity(self):
        journal, activities, member = self.execute()
        self.assertTrue(member.removed)
        self.assertTrue(all(activity.closed for activity in activities))
        affected = activities[1:5]
        for activity, role in zip(affected, ("source", "display", "audio", "switch_device")):
            remote = [selected for selected in activity.selections.values()
                      if isinstance(selected, Component) and selected.dut.name == "amity-dut-2"]
            self.assertEqual(remote, [activity.selections[role]])
        self.assertIsNone(affected[-1].selections["source"])
        self.assertEqual(affected[-1].selections["switch_input"], 3)
        positions = {name: next(index for index, row in enumerate(journal) if row[0] == name)
                     for name in ("stale_rejected", "confirm", "start", "standby", "close", "member_close", "bed_close")}
        second_keeper = next(index for index, row in enumerate(journal) if row == ("create", "Removal second keeper"))
        self.assertLess(second_keeper, positions["stale_rejected"])
        self.assertLess(positions["stale_rejected"], positions["confirm"])
        self.assertLess(positions["confirm"], positions["start"])
        self.assertLess(positions["standby"], positions["close"])
        self.assertLess(positions["close"], positions["member_close"])
        self.assertLess(positions["member_close"], positions["bed_close"])
        self.assertEqual(sum(row[0] == "review" for row in journal), 2)
        self.assertEqual(sum(row[0] == "confirm" for row in journal), 1)
        self.assertEqual(sum(row[0] == "power" for row in journal), 6)
        self.assertEqual(sum(row[0] == "location" for row in journal), 6)
        self.assertEqual(sum(row[0] == "route" for row in journal), 1)

    def test_failed_stale_or_final_confirmation_still_closes_owned_contexts(self):
        for fault in ("stale", "confirm"):
            with self.subTest(fault=fault):
                journal, activities, member = self.execute(fail=fault)
                self.assertFalse(member.removed)
                self.assertTrue(all(activity.closed for activity in activities))
                self.assertFalse(any(row[0] == "start" for row in journal))
                self.assertEqual(journal[-2:], [("member_close",), ("bed_close",)])


if __name__ == "__main__":
    unittest.main()
