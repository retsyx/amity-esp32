"""Endurance scheduling and scenario coverage without bench or browser I/O."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import Component, DUT, Key, PowerState, TvComponent, assign_addresses
from amity_harness.cec import LinuxCecBackend
from functional_tests import soak
from unit_tests.test_linux_backend import Kernel


class VirtualClock:
    def __init__(self):
        self.now = 100.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        if not 0 < seconds <= 1:
            raise AssertionError("The soak must bound idle health-check intervals to one second")
        self.sleeps.append(seconds)
        self.now += seconds


class SoakFixture:
    def __init__(self, *, setup_delay=0, failed_key=None, failed_ble=None,
                 failed_pair=False, failed_ui=False, stuck_key=False):
        self.clock = VirtualClock()
        self.setup_delay = setup_delay
        self.failed_key, self.failed_ui, self.stuck_key = failed_key, failed_ui, stuck_key
        self.failed_ble, self.failed_pair = failed_ble, failed_pair
        self.events, self.activities, self.key_calls = [], [], []
        self.api_key_calls, self.ble_key_calls = [], []
        self.keyboard = self.gate_cleared_for = None
        self.current = None
        self.session = SimpleNamespace(assert_healthy=self.health)
        self.setup_finished = None
        self.control_observations = []

    def health(self):
        self.events.append(("healthy", self.clock.now))

    @property
    def proof(self):
        return self.control_observations[0]["checks"][0]

    @contextmanager
    def bed(self, *resources, members, preserve_dut_on_failure):
        self.duts = tuple(item for item in resources if isinstance(item, DUT))
        self.components = tuple(item for item in resources if isinstance(item, Component))
        self.members = tuple(members)
        self.preserve_dut_on_failure = preserve_dut_on_failure
        assign_addresses(self.components)
        for dut in self.duts:
            dut._testbed = self
        self.events.append(("bed.open",))
        try:
            yield self
        except BaseException:
            self.events.append(("bed.failed",))
            raise
        finally:
            self.events.append(("bed.close",))
            for dut in self.duts:
                dut._testbed = None

    def add(self, main, member):
        if main is not self.duts[0] or member not in self.members:
            raise AssertionError("Only the original main can add the three owned members")
        self.clock.now += self.setup_delay
        self.events.append(("member.add", member.name))
        return SimpleNamespace(assert_connected=lambda: self.events.append(("member.connected", member.name)))

    def scan(self, main):
        if [row[1] for row in self.events if row[0] == "member.add"] != [dut.name for dut in self.members]:
            raise AssertionError("The scan must follow all three additions")
        self.clock.now += self.setup_delay
        self.events.append(("scan",))
        fixture = self

        class Scan:
            expected = fixture.components
            owner_boots = {dut.identity.device_id: str(index) * 32
                           for index, dut in enumerate(fixture.duts, 1)}

            def assert_contains_only(self, *components):
                if components != fixture.components:
                    raise AssertionError("Every fixture component must be covered by discovery")
                fixture.events.append(("scan.only", len(components)))

            def assert_on(self, component, endpoint):
                if endpoint is not component.endpoint:
                    raise AssertionError("Discovery must use the component's documented attachment")
                fixture.events.append(("scan.on", component.name, endpoint.dut.name))

            def validate_owners(self, components):
                if components != self.expected:
                    raise AssertionError("Every owner must retain its boot proof")
                fixture.events.append(("owners.validated",))

        return Scan()

    def activity(self, main, name, *, display, audio, source):
        if main is not self.duts[0]:
            raise AssertionError("Activities belong to the original main")
        self.clock.now += self.setup_delay
        fixture = self

        class Activity:
            selected = (display, audio, source)

            def start(self):
                return fixture.select(self, "api")

            def assert_active(self):
                if fixture.current is not self:
                    raise AssertionError("The expected activity must stay current")
                fixture.events.append(("activity.active", name))

        activity = Activity()
        activity.name = name
        self.activities.append(activity)
        self.events.append(("activity.create", name, source.dut.name))
        return activity

    def select(self, activity, route):
        if self.keyboard is not None and self.keyboard.held:
            raise AssertionError("Physical BLE input must be all up before an activity change")
        self.events.append(("select", activity.name, route, self.clock.now))
        if route == "ui" and self.failed_ui:
            raise TimeoutError("UI selection failed")
        previous = () if self.current is None else self.current.selected
        self.current = activity
        self.gate_cleared_for = None
        for component in self.components:
            component._power = PowerState.ON if component in activity.selected else PowerState.STANDBY
        action = SimpleNamespace(kind="select", selected=activity.selected, previous=previous)
        return action

    def assert_power(self, component, power):
        if component.power != power:
            raise AssertionError("The scenario expected the wrong physical power state")
        return component

    def wait_power(self, component, power, *, since, command=None):
        if since.kind == "standby":
            valid = power == PowerState.STANDBY and component in since.previous and command == "standby"
        else:
            valid = ((power == PowerState.ON and component in since.selected and component not in since.previous)
                     or (power == PowerState.STANDBY and component in since.previous
                         and component not in since.selected and command == "standby"))
        if not valid:
            raise AssertionError("Fresh power evidence must use the exact transition boundary")
        self.assert_power(component, power)
        self.events.append(("power.wait", since.kind, component.name, power))

    def route_tv(self, component, *, since):
        if component is not since.selected[0] or since.kind != "select":
            raise AssertionError("TV routing must follow this activity's fresh boundary")
        self.events.append(("route.tv", component.name))

    def route_avr(self, component, player, *, since):
        if (component, player) != since.selected[1:] or since.kind != "select":
            raise AssertionError("Receiver routing must select this activity's exact source")
        self.events.append(("route.avr", component.name, player.name))

    @contextmanager
    def preserve_shared(self, *components):
        fixture = self

        class Observation:
            def assert_preserved(self, *, since):
                expected = tuple(component for component in since.selected if component in since.previous)
                if components != expected:
                    raise AssertionError("Shared-device observation must cover the actual handoff")
                fixture.events.append(("shared.preserved", tuple(component.name for component in components)))

        yield Observation()

    def record_key(self, transport, key, duration_ms):
        if self.gate_cleared_for is not self.current:
            raise AssertionError("Accepted activity selections need a fresh physical all-up before keys")
        call = (self.current.name, key, duration_ms, self.clock.now, transport)
        self.key_calls.append(call)
        calls = self.ble_key_calls if transport == "ble" else self.api_key_calls
        calls.append(call)
        if transport == "api" and len(calls) == self.failed_key:
            raise TimeoutError("Key request failed")
        if transport == "ble" and len(calls) == self.failed_ble:
            raise TimeoutError("BLE input failed")
        if duration_ms:
            self.clock.sleep(duration_ms / 1000)

    def press(self, main, key, *, duration_ms):
        self.record_key("api", key, duration_ms)
        expected = self.current.selected[1 if key in (Key.VOLUME_UP, Key.VOLUME_DOWN) else 2]
        fixture = self

        class KeyAction:
            def assert_received_by(self, component):
                if component is not expected:
                    raise AssertionError("Source and audio keys must use their actual recipient")
                fixture.events.append(("key.received", component.dut.name, component.name, key, duration_ms))
                if fixture.stuck_key:
                    component.pressed_key = key

        return KeyAction()

    def pair(self, main):
        added = [row[1] for row in self.events if row[0] == "member.add"]
        if main is not self.duts[0] or added != [dut.name for dut in self.members]:
            raise AssertionError("BLE pairing must follow all direct member additions")
        self.events.append(("keyboard.pair",))
        self.clock.now += self.setup_delay
        if self.failed_pair:
            raise RuntimeError("BLE pairing failed")
        fixture = self

        class Keyboard:
            paired = held = False

            def __enter__(self):
                self.paired = True
                fixture.events.append(("keyboard.open",))
                return self

            def __exit__(self, *args):
                self.held = self.paired = False
                fixture.events.append(("keyboard.close",))

            @contextmanager
            def reports(self):
                if not self.paired or fixture.current is None:
                    raise AssertionError("Physical rest reports require a paired keyboard and settled activity")
                fixture.events.append(("ble.reports.open", fixture.current.name))
                try:
                    yield self
                finally:
                    self.held = False
                    fixture.events.append(("ble.reports.close", fixture.current.name))

            def all_up(self):
                self.held = False
                fixture.gate_cleared_for = fixture.current
                fixture.events.append(("ble.all_up", fixture.current.name, fixture.clock.now))

        self.keyboard = Keyboard()
        return self.keyboard

    @contextmanager
    def ble_evidence(self, main, scan, *components):
        if main is not self.duts[0] or components != self.components or scan.expected != self.components:
            raise AssertionError("BLE evidence must observe all four owners' actual scanned recipients")
        fixture = self

        class Evidence:
            def assert_key(self, keyboard, component, *, key, keyboard_usage, consumer_usage, duration_ms):
                if keyboard is not fixture.keyboard or not keyboard.paired:
                    raise AssertionError("BLE input must use the owned Pi keyboard")
                expected_usage = {Key.RIGHT: (0x4F, None), Key.LEFT: (0x50, None),
                                  Key.VOLUME_UP: (None, 0xE9), Key.VOLUME_DOWN: (None, 0xEA)}[key]
                if (keyboard_usage, consumer_usage) != expected_usage:
                    raise AssertionError("The soak must emit the original HID keyboard/consumer usage")
                expected = fixture.current.selected[1 if consumer_usage is not None else 2]
                if component is not expected:
                    raise AssertionError("BLE keys must reach the selected source or audio owner")
                try:
                    keyboard.held = True
                    fixture.record_key("ble", key, duration_ms)
                    fixture.events.append(("key.received", component.dut.name, component.name, key, duration_ms))
                    if fixture.stuck_key:
                        component.pressed_key = key
                finally:
                    keyboard.held = False

        yield Evidence()

    def browser(self, main, activities):
        if tuple(activities) != tuple(self.activities):
            raise AssertionError("The browser must see all eight saved activities")
        fixture = self

        class Browser:
            report = {"selections": 0, "standbys": 0, "device_inspections": 0}

            def __enter__(self):
                fixture.clock.now += fixture.setup_delay
                fixture.events.append(("browser.open",))
                return self

            def __exit__(self, *args):
                fixture.events.append(("browser.close",))

            def inspect_devices(self):
                self.report["device_inspections"] += 1
                fixture.events.append(("browser.devices",))
                if fixture.setup_finished is None:
                    fixture.clock.now += fixture.setup_delay
                    fixture.setup_finished = fixture.clock.now

            def select(self, activity):
                action = fixture.select(activity, "ui")
                self.report["selections"] += 1
                return action

            def assert_active(self, activity):
                activity.assert_active()
                fixture.events.append(("browser.active", activity.name))

            def standby(self):
                if fixture.keyboard.held:
                    raise AssertionError("Physical BLE input must be all up before standby")
                self.report["standbys"] += 1
                previous = fixture.current.selected
                fixture.current = None
                for component in fixture.components:
                    component._power = PowerState.STANDBY
                fixture.events.append(("standby", fixture.clock.now))
                return SimpleNamespace(kind="standby", previous=previous)

        return Browser()

    def wait_for(self, main, predicate):
        state = {"current_activity": -1, "activity_settling": False}
        if self.current is not None or not predicate(state):
            raise AssertionError("The sweep must finish in settled standby")
        self.events.append(("standby.settled",))
        return state

    def run(self, test=soak.test_soak, *, duration=None):
        with ExitStack() as patches:
            for name, value in (("TestBed", self.bed), ("SoakBrowser", self.browser),
                                ("BLEInputEvidence", self.ble_evidence),
                                ("observe_no_standby", self.preserve_shared), ("event", lambda *args, **kwargs: None)):
                patches.enter_context(patch.object(soak, name, value))
            patches.enter_context(patch.object(soak, "time", self.clock))
            if duration is not None:
                patches.enter_context(patch.object(soak, "DURATION_SECONDS", duration))
            for cls, name, implementation in (
                    (DUT, "add_amity", self.add), (DUT, "scan_hdmi", self.scan),
                    (DUT, "pair_ble_keyboard", self.pair),
                    (DUT, "create_activity", self.activity), (DUT, "press_key", self.press),
                    (DUT, "wait_for", self.wait_for), (Component, "assert_power", self.assert_power),
                    (Component, "wait_for_power", self.wait_power),
                    (TvComponent, "wait_for_amity_input", self.route_tv),
                    (Component, "wait_for_input", self.route_avr)):
                patches.enter_context(patch.object(cls, name,
                                                  lambda resource, *args, _call=implementation, **kwargs:
                                                  _call(resource, *args, **kwargs)))
            test()


class SoakScenarioTests(unittest.TestCase):
    def test_all_four_systems_fit_linux_allocation_rules_on_each_documented_wire(self):
        owners = tuple(DUT(name) for name in
                       ("amity-dut", "amity-dut-2", "amity-dut-3", "amity-dut-4"))
        systems = [soak._system(dut, number) for number, dut in enumerate(owners, 1)]
        components = tuple(component for tv, avr, players in systems
                           for component in (tv, avr, *players))
        assign_addresses(components)
        wires = {}
        for component in components:
            wires.setdefault(component.gpio, []).append(component)
        self.assertEqual(len(wires), 8)
        for gpio, wire_components in wires.items():
            with self.subTest(gpio=gpio):
                types = [component.logical_address_type for component in wire_components]
                self.assertEqual(len(set(types)), len(types), "Linux forbids duplicate allocation types")
                self.assertFalse({1, 3} <= set(types), "Linux forbids combined recording/playback pools")
                backend = LinuxCecBackend(gpio)
                backend.fd = 123
                backend.caps = SimpleNamespace(available_log_addrs=4)
                kernel = Kernel()
                with patch("amity_harness.cec._ioctl", side_effect=kernel.ioctl):
                    backend.configure(wire_components)
                self.assertEqual(len(kernel.requests), 1)
                self.assertEqual(kernel.requests[0]["types"], types)

    def test_two_hour_workload_starts_after_setup_and_completes_every_sweep(self):
        fixture = SoakFixture(setup_delay=1000)
        fixture.run()
        self.assertEqual(soak.DURATION_SECONDS, 7200)
        self.assertGreater(fixture.setup_finished, 7200)
        self.assertEqual(fixture.clock.now - fixture.setup_finished, 7200)
        self.assertEqual(fixture.proof["elapsed_seconds"], 7200)
        self.assertTrue(fixture.proof["completed"])
        self.assertEqual(fixture.proof["sweeps"], 45)
        self.assertEqual(fixture.proof["activity_changes"], 45 * 8)
        self.assertEqual(fixture.proof["key_presses"], 45 * 8 * 4)
        self.assertEqual(fixture.proof["api_key_presses"], 45 * 8 * 2)
        self.assertEqual(fixture.proof["ble_key_presses"], 45 * 8 * 2)
        self.assertAlmostEqual(sum(fixture.clock.sleeps), 7200, places=6)
        self.assertEqual(sum(row[0] == "standby" for row in fixture.events), 45)
        self.assertEqual(sum(row[0] == "owners.validated" for row in fixture.events), 45)
        self.assertTrue(fixture.preserve_dut_on_failure)
        self.assertEqual(fixture.events[-3:], [("browser.close",), ("keyboard.close",), ("bed.close",)])
        self.assertEqual(sum(row[0] == "keyboard.pair" for row in fixture.events), 1)
        self.assertFalse(fixture.keyboard.paired)
        self.assertFalse(fixture.keyboard.held)

    def test_partial_deadline_finishes_sweep_with_all_owners_activities_and_routes(self):
        cases = (
            (soak.test_soak, ("amity-dut", "amity-dut-2", "amity-dut-3", "amity-dut-4")),
            (soak.test_soak_main4, ("amity-dut-4", "amity-dut", "amity-dut-2", "amity-dut-3")),
        )
        for test, names in cases:
            with self.subTest(test=test.__name__):
                fixture = SoakFixture()
                fixture.run(test, duration=161)
                self.assertEqual(tuple(dut.name for dut in fixture.duts), names)
                self.assertEqual(tuple(dut.name for dut in fixture.members), names[1:])
                self.assertEqual(len(fixture.components), 16)
                self.assertEqual(len(fixture.activities), 8)
                self.assertEqual(fixture.proof["elapsed_seconds"], 320)
                self.assertEqual(fixture.proof["sweeps"], 2)
                selects = [row for row in fixture.events if row[0] == "select"]
                for index, activity in enumerate(fixture.activities):
                    routes = [row[2] for row in selects if row[1] == activity.name]
                    self.assertEqual(set(routes), {"api", "ui"})
                    self.assertEqual(routes, ["ui", "api"] if index % 2 == 0 else ["api", "ui"])
                self.assertEqual(fixture.proof["ui_activity_changes"], 8)
                self.assertEqual(fixture.proof["api_key_presses"], 32)
                self.assertEqual(fixture.proof["ble_key_presses"], 32)
                self.assertEqual(sum(row[0] == "ble.all_up" for row in fixture.events), 16)
                # Every saved activity uses both transports for every navigation/audio
                # key across consecutive sweeps, including taps and held reports.
                for activity in fixture.activities:
                    calls = [row for row in fixture.key_calls if row[0] == activity.name]
                    for key in (Key.RIGHT, Key.LEFT, Key.VOLUME_UP, Key.VOLUME_DOWN):
                        self.assertEqual({row[4] for row in calls if row[1] == key}, {"api", "ble"})
                self.assertEqual(sum(row[0] == "scan" for row in fixture.events), 1)
                self.assertEqual(sum(row[0] == "scan.on" for row in fixture.events), 16)
                self.assertEqual(sum(row[0] == "route.tv" for row in fixture.events), 16)
                self.assertEqual(sum(row[0] == "route.avr" for row in fixture.events), 16)
                shared = [row for row in fixture.events if row[0] == "shared.preserved"]
                self.assertEqual(len(shared), 8)
                self.assertTrue(all(len(row[1]) == 2 for row in shared))
                keys = [row for row in fixture.events if row[0] == "key.received"]
                for owner in names:
                    own = [row for row in keys if row[1] == owner]
                    self.assertEqual(len(own), 16)
                    self.assertEqual({(row[3], row[4]) for row in own}, {
                        (Key.RIGHT, 0), (Key.LEFT, 650), (Key.VOLUME_UP, 750), (Key.VOLUME_DOWN, 0)})
                self.assertTrue(all(component.power == PowerState.STANDBY for component in fixture.components))
                self.assertIsNone(fixture.current)

    def test_periodic_keys_keep_five_second_spacing_within_each_activity(self):
        fixture = SoakFixture()
        fixture.run(duration=1)
        for activity in fixture.activities:
            calls = [row for row in fixture.key_calls if row[0] == activity.name]
            self.assertEqual(len(calls), 4)
            self.assertEqual([row[3] - calls[0][3] for row in calls], [0, 5, 10, 15])

    def test_failed_key_is_not_retried_or_hidden_and_retains_incomplete_proof(self):
        fixture = SoakFixture(failed_key=1)
        with self.assertRaisesRegex(TimeoutError, "Key request failed"):
            fixture.run(duration=1)
        self.assertEqual(len(fixture.key_calls), 2)
        self.assertEqual(len(fixture.api_key_calls), 1)
        self.assertEqual(len(fixture.ble_key_calls), 1)
        self.assertEqual(fixture.proof["key_presses"], 1)
        self.assertEqual(fixture.proof["api_key_presses"], 0)
        self.assertEqual(fixture.proof["ble_key_presses"], 1)
        self.assertEqual(fixture.proof["activity_changes"], 1)
        self.assertFalse(fixture.proof["completed"])
        self.assertEqual(fixture.proof["elapsed_seconds"], 5)
        self.assertFalse(any(row[0] == "standby" for row in fixture.events))
        self.assertEqual(fixture.events[-4:], [
            ("browser.close",), ("keyboard.close",), ("bed.failed",), ("bed.close",)])

    def test_ble_failure_never_falls_back_to_http_and_always_closes_owned_keyboard(self):
        fixture = SoakFixture(failed_ble=1)
        with self.assertRaisesRegex(TimeoutError, "BLE input failed"):
            fixture.run(duration=1)
        self.assertEqual(len(fixture.ble_key_calls), 1)
        self.assertEqual(fixture.api_key_calls, [])
        self.assertEqual(fixture.proof["key_presses"], 0)
        self.assertEqual(fixture.proof["api_key_presses"], 0)
        self.assertEqual(fixture.proof["ble_key_presses"], 0)
        self.assertFalse(fixture.proof["completed"])
        self.assertFalse(fixture.keyboard.paired)
        self.assertFalse(fixture.keyboard.held)
        self.assertEqual(fixture.events[-4:], [
            ("browser.close",), ("keyboard.close",), ("bed.failed",), ("bed.close",)])

    def test_pairing_failure_prevents_workload_instead_of_using_api_only(self):
        fixture = SoakFixture(failed_pair=True)
        with self.assertRaisesRegex(RuntimeError, "BLE pairing failed"):
            fixture.run(duration=1)
        self.assertEqual(fixture.key_calls, [])
        self.assertFalse(any(row[0] == "browser.open" for row in fixture.events))
        self.assertFalse(fixture.proof["completed"])

    def test_failed_ui_selection_does_not_fall_back_to_api(self):
        fixture = SoakFixture(failed_ui=True)
        with self.assertRaisesRegex(TimeoutError, "UI selection failed"):
            fixture.run(duration=1)
        self.assertEqual([row[2] for row in fixture.events if row[0] == "select"], ["ui"])
        self.assertEqual(fixture.key_calls, [])
        self.assertEqual(fixture.proof["activity_changes"], 0)
        self.assertFalse(fixture.proof["completed"])

    def test_stuck_key_fails_immediately_instead_of_changing_activity(self):
        fixture = SoakFixture(stuck_key=True)
        with self.assertRaisesRegex(AssertionError, "retained a pressed key"):
            fixture.run(duration=1)
        self.assertEqual(len(fixture.key_calls), 1)
        self.assertEqual(fixture.proof["activity_changes"], 1)
        self.assertFalse(fixture.proof["completed"])


if __name__ == "__main__":
    unittest.main()
