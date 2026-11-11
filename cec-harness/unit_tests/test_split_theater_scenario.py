"""An independent main can finish while the source owner's lookup is pending."""

from contextlib import ExitStack, nullcontext
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Component, DUT, PlaybackComponent, PowerState, TvComponent
from amity_harness.activity import PendingActivity
from functional_tests import split_theater
from unit_tests.test_consortium_scenarios import Scenario


class SplitScenario(Scenario):
    """Logical wire times preserve a slow lookup and short command deadlines."""

    def __init__(self, *, missing_source_power=False):
        super().__init__()
        self.now = 0
        self.lookup_done = 3
        self.source_power_at = None if missing_source_power else 3.1
        self.released_at = None
        self.scan_count = 0
        self.missing = None

    def address(self, *args):
        return nullcontext(SimpleNamespace(assert_address=lambda **kw: None))

    def add(self, *args):
        member = super().add(*args)
        member.front_physical_address = self.address
        return member

    def scan(self, main):
        self.scan_count += 1
        scan = super().scan(main)
        original = scan.assert_contains

        def contains(*components):
            if scan is self.missing and self.source in components:
                if self.released_at is None:
                    raise AssertionError("The source name is still withheld")
                self.now = max(self.now, self.lookup_done)
            return original(*components)

        scan.assert_contains = contains
        return scan

    def gate(self, source, initial, *, progress):
        fixture = self

        class Gate:
            released_at = None

            def set_missing_scan(self, scan):
                fixture.missing = scan

            def wait_for_progress(self, *, since):
                if fixture.now >= fixture.lookup_done or fixture.released_at is not None:
                    raise AssertionError("The other owner must progress during the lookup")
                for component in progress:
                    if component.power != PowerState.ON:
                        raise AssertionError("The other owner's physical power did not progress")

            def release(self):
                self.released_at = fixture.released_at = .5

        return nullcontext(Gate())

    def activity(self, main, name, **selections):
        activity = super().activity(main, name, **selections)
        self.source = selections["source"]

        def pending(*, scan):
            self.current = activity
            for component in (selections["display"], selections["audio"]):
                component._power = PowerState.ON
            self.action = SimpleNamespace(kind="start", started_ns=0)

            def finish():
                # A main-owned source holds the main's operation open. A
                # member-owned source cannot hold the main's completion open.
                self.now = self.lookup_done if self.source.dut is main else .5
                return self.action

            return PendingActivity(self.action, finish)

        activity.start_pending = pending
        activity.refresh_scan = lambda scan: None
        return activity

    def wait_power(self, component, power, *, since, command=None):
        if power == PowerState.ON and component is self.source:
            if since != self.released_at:
                raise AssertionError("Source power needs the original physical name-reply boundary")
            deadline = self.now + 1
            if self.source_power_at is None or self.source_power_at > deadline:
                raise TimeoutError("No source power command within the CEC deadline")
            if self.source_power_at < since:
                raise AssertionError("The source power command is stale")
            self.now = max(self.now, self.source_power_at)
            component._power = power
        self.assert_power(component, power)
        return component

    def run(self, test):
        with ExitStack() as patches:
            for target, name, replacement in (
                (split_theater, "TestBed", self.bed),
                (split_theater, "LookupNameGate", self.gate),
                (split_theater, "LookupScans", lambda *args: nullcontext(
                    SimpleNamespace(wait_for_cooldown=lambda: None))),
                (DUT, "add_amity", self.add),
                (DUT, "scan_hdmi", self.scan),
                (DUT, "create_activity", self.activity),
                (DUT, "front_physical_address", self.address),
                (DUT, "press_key", lambda *args: SimpleNamespace(assert_received_by=lambda recipient: None)),
                (Component, "wait_for_power", self.wait_power),
                (Component, "assert_power", self.assert_power),
                (TvComponent, "wait_for_amity_input", lambda *args, **kw: None),
                (Component, "wait_for_active_source", lambda *args, **kw: None),
                (PlaybackComponent, "wait_for_source_request", lambda *args, **kw: None),
            ):
                patches.enter_context(patch.object(target, name,
                    lambda *args, method=replacement, **kw: method(*args, **kw)))
            test()


class SplitScenarioTests(unittest.TestCase):
    def test_both_owner_orders_keep_lookup_wait_separate_from_cec_deadline(self):
        for test in (split_theater.test_split_theater_main_source,
                     split_theater.test_split_theater_member_source):
            with self.subTest(test=test.__name__):
                fixture = SplitScenario()
                fixture.run(test)
                self.assertEqual(fixture.now, fixture.source_power_at)
                # Initial discovery, missing-name scan and final cleanup only:
                # awaiting the original lookup must never enumerate again.
                self.assertEqual(fixture.scan_count, 3)

    def test_completed_lookup_cannot_replace_a_fresh_source_power_command(self):
        fixture = SplitScenario(missing_source_power=True)
        with self.assertRaisesRegex(TimeoutError, "No source power command"):
            fixture.run(split_theater.test_split_theater_member_source)
        self.assertEqual(fixture.now, fixture.lookup_done)


if __name__ == "__main__":
    unittest.main()
