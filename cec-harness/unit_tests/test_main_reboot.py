"""A main-only reboot leaves members quiet and moves only the main's boot proofs."""

from copy import deepcopy
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecAddress, DUT, Frame, PlaybackComponent, TvComponent, TxResult
from amity_harness.config import RunConfig, use_config
from amity_harness.reboot import MainReboot
from unit_tests.virtual_time import use_virtual_time


class MainRebootFixture:
    def __init__(self):
        with use_config(RunConfig(insecure=True)):
            self.main, self.secondary = DUT("amity-dut"), DUT("amity-dut-4")
        self.owners = (self.main, self.secondary)
        self.main_id, self.member_id = self.main.identity.device_id, self.secondary.identity.device_id
        self.boots = {self.main_id: "1" * 32, self.member_id: "2" * 32}
        self.new_boot = "3" * 32
        self.group = "4" * 32
        self.events, self.callbacks, self.requests = [], [], []
        self.on_usb = lambda: None
        self.on_poll = lambda path: None
        self.offline = 0
        self.current, self.settling, self.error = 0, False, ""
        self.roster = [self.member_id]
        self.config = {
            "version": 4, "consortium_id": self.group, "controller_octopus_id": self.main_id,
            "config_revision": "7", "presentation_revision": "8",
            "octopuses": [{"octopus_id": dut.identity.device_id, "supported_tentacles": 4} for dut in self.owners],
            "activities": [{"activity_id": "9" * 32, "label": "Movie"}, {"activity_id": "8" * 32, "label": "Game"}],
            "components": [{"component_id": "a" * 32, "label": "Player"}],
        }
        self.settings_changes = {}
        self.tv = TvComponent(self.main.tentacle(0).front, "TV", address=CecAddress(0, 0), power="on")
        self.avr = AvrComponent(self.secondary.tentacle(0).front, "AVR",
                                address=CecAddress(5, 0x1000), power="on")
        self.player = PlaybackComponent(self.secondary.tentacle(1).back, "Player",
                                        address=CecAddress(4, 0x1300), power="on")
        self.screen = TvComponent(self.secondary.tentacle(1).front, "Screen", address=CecAddress(0, 0))
        self.components = (self.tv, self.avr, self.screen, self.player)
        # Member initiators per wire, as a scan would prove them.
        self.initiators = {self.tv.gpio: 4, self.avr.gpio: 8, self.screen.gpio: 4, self.player.gpio: 0}
        self.respond = None
        self.tv.active_input = 0x1000
        self.avr.active_input = self.player.address.physical
        # Component replies to DUT queries complete as physical transmissions.
        self.session = SimpleNamespace(components=self.components, _lock=threading.RLock(),
            _stop=threading.Event(), _record=lambda *args, **kwargs: None,
            power_changed=lambda *args, **kwargs: None, assert_healthy=lambda: None,
            transmit=self.transmit,
            _wire_idle_since_ns={component.gpio: 10**30 for component in self.components})
        self.bed = SimpleNamespace(_entered=True, _closed=False, session=self.session,
            components=self.components, duts=self.owners, members=(self.secondary,), _member_proofs={},
            add_cleanup=self.callbacks.append, control_observations=[])
        for dut in self.owners:
            dut._testbed = self.bed
        for component in self.components:
            component._session = self.session
        self.member = SimpleNamespace(main=self.main, candidate=self.secondary, name=self.secondary.name,
            session=self.session, device_id=self.member_id, _created=True, _closed=False,
            boot=self.boots[self.main_id], member_boot=self.boots[self.member_id], group=self.group,
            _initial_system={"boot_id": self.boots[self.main_id], "membership": {"peers": []}})
        self.bed._member_proofs[self.member_id] = self.member
        self.main.request = self.request
        self.main.authenticate = lambda: None
        self.main.scan_hdmi = self.scan
        self.main.usb_reboot = self.usb_reboot
        self.main.last_scan = self.make_scan()
        self.movie = self.activity("9" * 32, "Movie", source=self.player)
        self.game = self.activity("8" * 32, "Game", source=None)
        self.rows = [deepcopy(self.movie._created_row), deepcopy(self.game._created_row)]

    def activity(self, identity, name, *, source):
        activity = SimpleNamespace(dut=self.main, session=self.session, _testbed=self.bed,
            _created=True, _closed=False, activity_id=identity, name=name,
            _created_row={"activity_id": identity, "name": name},
            boot=self.boots[self.main_id], _saved_group=self.group,
            components={"display": self.tv, "audio": self.avr, "source": source, "switch": None},
            scan=self.main.last_scan)
        activity.assert_active = lambda: self.assert_active(activity)
        activity.refresh_scan = lambda scan: self.refresh_scan(activity, scan)
        return activity

    def system(self):
        peers = [{"octopus_id": owner, "hostname": self.secondary.name, "boot_id": self.boots.get(owner),
                  "enrolled": True, "ready": True, "supported_tentacles": 4} for owner in self.roster]
        return {"octopus_id": self.main_id, "consortium_id": self.group, "boot_id": self.boots[self.main_id],
            "hostname": self.main.name, "mode": "coordinator", "ready": True, "configuration_pending": False,
            "applied_config_revision": self.config["config_revision"],
            "applied_presentation_revision": self.config["presentation_revision"],
            "membership": {"coordinator": True, "member": False, "authority_ready": True, "peers": peers}}

    def request(self, path, data=None, **kwargs):
        self.requests.append((path, deepcopy(data)))
        if data is not None:
            raise AssertionError("A main reboot must not mutate the shipping UI state")
        if self.offline:
            self.offline -= 1
            raise ConnectionRefusedError("The main is still booting")
        self.on_poll(path)
        if path == "/api/system":
            return self.system()
        if path == "/api/config":
            return deepcopy(self.config)
        if path == "/api/activity-editor":
            return {"activities": deepcopy(self.rows)}
        if path == "/api/activity-editor/status":
            return {"expected_boot_id": self.boots[self.main_id], "current_activity": self.current,
                    "activity_settling": self.settling, "activity_error": self.error}
        if path.startswith("/api/devices/") and path.endswith("/settings"):
            owner = path.split("/")[3]
            settings = {"boot_id": self.boots[owner], "hostname": "name-" + owner[-4:],
                "hostname_revision": "1", "settings_revision": "2",
                "adapters": [{"tentacle_id": index, "configured": True,
                              "default_front_physical_address": "1.0.0.0"} for index in range(4)]}
            settings.update(deepcopy(self.settings_changes.get(owner, {})))
            return {"status": "ready", "error": "", "settings": settings}
        raise AssertionError("Unexpected shipping endpoint: " + path)

    def transmit(self, component, frame):
        result = TxResult(frame, 1, time.monotonic_ns())
        if frame.opcode == 0x82 and self.respond is not None:
            self.respond(component, frame)
        return result

    def no_activity_policy(self, front, back, *, extra=None):
        """Answer a back Active Source the way an owner with nothing current does."""
        def respond(component, frame):
            if component is back:
                self.receive(front, 0x36, source=self.initiators[front.gpio], destination=15)
                self.receive(back, 0x36, source=self.initiators[back.gpio], destination=15)
                if extra is not None:
                    extra()
        self.respond = respond

    def assert_active(self, activity):
        self.events.append(("assert_active", activity.name))
        state = self.request("/api/activity-editor/status")
        if (state["expected_boot_id"] != activity.boot or state["current_activity"] != 0
                or state["activity_settling"] is not False):
            raise AssertionError("The movie is not stably current")
        return activity

    def make_scan(self):
        fixture = self

        class Scan:
            def __init__(self):
                self.dut, self._session, self._testbed = fixture.main, fixture.session, fixture.bed
                self._boot = fixture.boots[fixture.main_id]
                self._group = fixture.group
                self.expected = fixture.components
                self.started = time.monotonic_ns()
                self.owner_duts = {dut.identity.device_id: dut for dut in fixture.owners}
                self.owner_boots = dict(fixture.boots)
                self.initiators = dict(fixture.initiators)

            def validate_owners(self, components):
                if (self._boot != fixture.boots[fixture.main_id] or fixture.member.boot != self._boot
                        or fixture.member.member_boot != fixture.boots[fixture.member_id]):
                    raise AssertionError("Stale owner boot evidence")

            def assert_contains(self, *components):
                if components != fixture.components:
                    raise AssertionError("The fresh scan lost fixture equipment")

            assert_contains_only = assert_contains

        return Scan()

    def scan(self, **kwargs):
        # The scan makes members enumerate: it must follow the quiet proof.
        observed = any(component._wire_observers for component in self.components)
        self.events.append(("scan", observed))
        self.main.last_scan = self.make_scan()
        self.main.last_scan.validate_owners(self.components)
        return self.main.last_scan

    def refresh_scan(self, activity, scan):
        self.events.append(("refresh_scan", activity.name))
        scan.validate_owners(self.components)
        activity.scan = scan

    def usb_reboot(self):
        self.events.append(("usb_reboot",))
        time.sleep(0.01)
        self.boots[self.main_id] = self.new_boot
        self.current = -1
        self.offline = 3
        self.on_usb()
        return "/dev/ttyACM-test"

    def receive(self, component, opcode, operands=b"", *, source=8, destination=None, timestamp_ns=None):
        time.sleep(0.001)
        frame = Frame(source, component.address.logical if destination is None else destination,
            opcode, operands, timestamp_ns=time.monotonic_ns() if timestamp_ns is None else timestamp_ns)
        component.receive(frame)

    def reboot(self):
        return MainReboot(self.movie, (self.member,), saved=(self.game,), hold=3)


class MainRebootTests(unittest.TestCase):
    def setUp(self):
        # Polling, reconnection and drain waits share one virtual clock.
        self.clock = use_virtual_time(self)

    def assert_unbound(self, fixture):
        old = fixture.member._initial_system["boot_id"]
        self.assertEqual(fixture.member.boot, old)
        self.assertEqual((fixture.movie.boot, fixture.game.boot), (old, old))
        self.assertFalse(any(event[0] == "scan" for event in fixture.events))
        self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_one_reboot_proves_quiet_members_before_moving_main_boot_proofs(self):
        fixture = MainRebootFixture()
        old_scan, member_boot = fixture.main.last_scan, fixture.member.member_boot
        # A booting main may announce itself; it must not command its equipment.
        fixture.on_usb = lambda: fixture.receive(fixture.tv, 0x84, b"\x10\x00\x04", source=4, destination=15)
        with fixture.reboot() as reboot:
            self.assertIs(reboot.reboot(), reboot)
            with self.assertRaisesRegex(RuntimeError, "exactly one"):
                reboot.reboot()
        self.assertEqual(fixture.events.count(("usb_reboot",)), 1)
        self.assertEqual([event for event in fixture.events if event[0] == "scan"], [("scan", False)])
        self.assertEqual([event for event in fixture.events if event[0] == "refresh_scan"],
                         [("refresh_scan", "Movie"), ("refresh_scan", "Game")])
        self.assertEqual(fixture.member.boot, fixture.new_boot)
        self.assertEqual(fixture.member._initial_system["boot_id"], fixture.new_boot)
        self.assertEqual(fixture.member.member_boot, member_boot)
        self.assertEqual((fixture.movie.boot, fixture.game.boot), (fixture.new_boot, fixture.new_boot))
        self.assertIsNot(fixture.movie.scan, old_scan)
        self.assertIs(fixture.game.scan, fixture.movie.scan)
        self.assertFalse(any(body is not None for _, body in fixture.requests))
        self.assertTrue(all(not component._wire_observers for component in fixture.components))
        checks = [check["kind"] for check in fixture.bed.control_observations[0]["checks"]]
        self.assertEqual(checks, ["observing", "usb_reboot", "members_quiet", "rebooted"])
        report = fixture.bed.control_observations[0]
        self.assertEqual(report["wires"], sorted((fixture.avr.gpio, fixture.screen.gpio, fixture.player.gpio)))
        self.assertEqual(report["checks"][0]["main_wires"], [fixture.tv.gpio])
        self.assertEqual(report["main_boot"], fixture.new_boot)
        quiet = report["checks"][2]
        self.assertEqual((quiet["member_frame_count"], quiet["main_frame_count"]), (0, 1))

    def test_any_member_frame_fails_before_rebinding(self):
        cases = ((lambda f: f.receive(f.avr, 0x8f)), (lambda f: f.receive(f.player, 0x36, destination=15)),
                 (lambda f: f.receive(f.player, 0x83)), (lambda f: f.receive(f.avr, 0x71)))
        for index, send in enumerate(cases):
            with self.subTest(case=index):
                fixture = MainRebootFixture()
                fixture.on_usb = lambda: send(fixture)
                with self.assertRaisesRegex(AssertionError, "A member sent CEC"):
                    with fixture.reboot() as reboot:
                        reboot.reboot()
                self.assert_unbound(fixture)

    def test_a_member_frame_after_the_ready_proof_still_fails(self):
        fixture = MainRebootFixture()
        sent = []

        def late_frame(path):
            if path.endswith(fixture.member_id + "/settings") and fixture.boots[fixture.main_id] == fixture.new_boot \
                    and not sent:
                sent.append(True)
                fixture.receive(fixture.player, 0x36, destination=15)

        fixture.on_poll = late_frame
        with self.assertRaisesRegex(AssertionError, "A member sent CEC"):
            with fixture.reboot() as reboot:
                reboot.reboot()
        self.assert_unbound(fixture)

    def test_the_quiet_window_covers_a_cec_response_time_after_ready(self):
        fixture = MainRebootFixture()
        late = []

        def frame_soon_after_ready(path):
            # The last read before the drain proves the saved activities.
            if path == "/api/activity-editor" and fixture.boots[fixture.main_id] == fixture.new_boot and not late:
                def send():
                    time.sleep(0.5)
                    fixture.receive(fixture.player, 0x36, destination=15)
                late.append(threading.Thread(target=send, daemon=True))
                late[0].start()

        fixture.on_poll = frame_soon_after_ready
        with self.assertRaisesRegex(AssertionError, "A member sent CEC"):
            with fixture.reboot() as reboot:
                reboot.reboot()
        late[0].join()
        self.assert_unbound(fixture)

    def test_booting_main_must_not_command_its_equipment(self):
        for opcode, operands, destination in ((0x36, b"", 15), (0x04, b"", 0), (0x82, b"\x10\x00", 15),
                                              (0x86, b"\x20\x00", 15), (0x44, b"\x6c", 0)):
            with self.subTest(opcode=opcode, operands=operands):
                fixture = MainRebootFixture()
                fixture.on_usb = lambda: fixture.receive(fixture.tv, opcode, operands, source=4,
                                                         destination=destination)
                with self.assertRaisesRegex(AssertionError, "commanded its equipment"):
                    with fixture.reboot() as reboot:
                        reboot.reboot()
                self.assert_unbound(fixture)

    def test_equipment_state_must_not_change_during_the_outage(self):
        fixture = MainRebootFixture()
        fixture.on_usb = lambda: fixture.avr.standby()
        with self.assertRaisesRegex(AssertionError, "changed power or input"):
            with fixture.reboot() as reboot:
                reboot.reboot()
        self.assert_unbound(fixture)

    def test_the_main_must_boot_with_nothing_current(self):
        fixture = MainRebootFixture()
        fixture.on_usb = lambda: setattr(fixture, "current", 0)
        with self.assertRaisesRegex(AssertionError, "restored an activity"):
            with fixture.reboot() as reboot:
                reboot.reboot()
        self.assert_unbound(fixture)

    def test_a_member_that_never_reconnects_is_not_recovered(self):
        fixture = MainRebootFixture()
        fixture.on_usb = lambda: fixture.boots.__setitem__(fixture.member_id, None)
        with self.assertRaises(TimeoutError):
            with fixture.reboot() as reboot:
                reboot.reboot()
        self.assert_unbound(fixture)

    def test_quiet_window_does_not_wait_for_ui_settling(self):
        fixture = MainRebootFixture()
        fixture.on_usb = lambda: setattr(fixture, "settling", True)
        with fixture.reboot() as reboot:
            reboot.reboot()
        self.assertEqual(fixture.member.boot, fixture.new_boot)

    def test_member_restart_roster_change_or_second_main_restart_fails(self):
        for case, message in (("member", "A member restarted"), ("roster", "roster"),
                              ("again", "restarted again")):
            with self.subTest(case=case):
                fixture = MainRebootFixture()
                if case == "member":
                    fixture.on_usb = lambda: fixture.boots.__setitem__(fixture.member_id, "b" * 32)
                elif case == "roster":
                    fixture.on_usb = lambda: fixture.roster.clear()
                else:
                    def restart_again(path):
                        # The new boot restarts while it is being verified.
                        if path == "/api/config" and fixture.boots[fixture.main_id] == fixture.new_boot:
                            fixture.boots[fixture.main_id] = "c" * 32
                    fixture.on_poll = restart_again
                with self.assertRaisesRegex(AssertionError, message):
                    with fixture.reboot() as reboot:
                        reboot.reboot()
                self.assert_unbound(fixture)

    def test_saved_configuration_activities_and_settings_must_survive(self):
        for changed, message in (("configuration", "changed saved equipment"),
                                 ("settings", "changed saved device settings"),
                                 ("activity", "saved activity changed")):
            with self.subTest(changed=changed):
                fixture = MainRebootFixture()

                def mutate():
                    if changed == "configuration":
                        fixture.config["components"][0]["label"] = "Changed equipment"
                    elif changed == "settings":
                        fixture.settings_changes[fixture.member_id] = {
                            "adapters": [{"tentacle_id": 0, "configured": False,
                                          "default_front_physical_address": "2.0.0.0"}]}
                    else:
                        fixture.rows[1]["name"] = "Edited"

                fixture.on_usb = mutate
                with self.assertRaisesRegex(AssertionError, message):
                    with fixture.reboot() as reboot:
                        reboot.reboot()
                self.assertFalse(any(event[0] == "scan" for event in fixture.events))

    def test_undrained_wires_cannot_prove_quiet_members(self):
        fixture = MainRebootFixture()
        fixture.session._wire_idle_since_ns = {}
        with self.assertRaises(TimeoutError):
            with fixture.reboot() as reboot:
                reboot.reboot()
        self.assert_unbound(fixture)

    def test_physical_evidence_requires_a_valid_wire_timestamp(self):
        fixture = MainRebootFixture()
        fixture.on_usb = lambda: fixture.receive(fixture.tv, 0x84, b"\x10\x00\x04", source=4, destination=15,
                                                 timestamp_ns=0)
        with self.assertRaisesRegex(AssertionError, "timestamped physical"):
            with fixture.reboot() as reboot:
                reboot.reboot()
        self.assert_unbound(fixture)

    def test_reboot_needs_the_current_activity_and_every_verified_member(self):
        fixture = MainRebootFixture()
        with self.assertRaisesRegex(ValueError, "every verified member"):
            MainReboot(fixture.movie, (), hold=3)
        for hold in (0, -1, float("inf"), True):
            with self.assertRaisesRegex(ValueError, "quiet hold"):
                MainReboot(fixture.movie, (fixture.member,), hold=hold)
        fixture.current = 1
        with self.assertRaisesRegex(AssertionError, "not stably current"):
            with fixture.reboot():
                pass
        fixture.current = 0
        with self.assertRaisesRegex(RuntimeError, "not verified"):
            with fixture.reboot():
                pass
        self.assertNotIn(("usb_reboot",), fixture.events)

    def rebooted(self):
        fixture = MainRebootFixture()
        reboot = fixture.reboot()
        reboot.__enter__()
        self.addCleanup(reboot.close)
        reboot.reboot()
        fixture.events.clear()
        return fixture, reboot

    def test_no_activity_policy_is_both_broadcasts_then_a_quiet_hold(self):
        fixture, reboot = self.rebooted()
        fixture.no_activity_policy(fixture.screen, fixture.player)
        self.assertIs(reboot.assert_no_activity_policy(fixture.screen, fixture.player), reboot)
        for component in (fixture.screen, fixture.player):
            self.assertEqual(component.power.value, "standby")
            proof = [check for check in component.power_assertions if check["kind"] == "wire_command"][-1]
            self.assertEqual((proof["command"], proof["destination"]), ("standby", 15))
        check = fixture.bed.control_observations[0]["checks"][-1]
        self.assertEqual(check["kind"], "no_activity_policy")
        self.assertEqual(check["observation_end_monotonic_ns"] - max(check["standby_wire_timestamp_ns"]), 3 * 10**9)
        self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_no_activity_policy_rejects_any_other_frame_within_the_hold(self):
        cases = ((lambda f: f.receive(f.screen, 0x04, source=4)),
                 (lambda f: f.receive(f.player, 0x36, source=0, destination=15)),
                 (lambda f: f.receive(f.avr, 0x71, source=8)),
                 (lambda f: f.receive(f.tv, 0x82, b"\x10\x00", source=4, destination=15)))
        for index, extra in enumerate(cases):
            with self.subTest(case=index):
                fixture, reboot = self.rebooted()
                late = []

                def schedule(fixture=fixture, extra=extra):
                    def send():
                        time.sleep(1)
                        extra(fixture)
                    late.append(threading.Thread(target=send, daemon=True))
                    late[0].start()

                fixture.no_activity_policy(fixture.screen, fixture.player, extra=schedule)
                with self.assertRaisesRegex(AssertionError, "more than the no-activity Standby"):
                    reboot.assert_no_activity_policy(fixture.screen, fixture.player)
                late[0].join()
                self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_no_activity_policy_needs_broadcast_standby_on_both_wires(self):
        for missing in ("front", "directed"):
            with self.subTest(missing=missing):
                fixture, reboot = self.rebooted()

                def respond(component, frame, fixture=fixture, missing=missing):
                    if component is fixture.player:
                        if missing == "directed":
                            fixture.receive(fixture.screen, 0x36, source=4, destination=0)
                        fixture.receive(fixture.player, 0x36, source=0, destination=15)

                fixture.respond = respond
                with self.assertRaises((AssertionError, TimeoutError)):
                    reboot.assert_no_activity_policy(fixture.screen, fixture.player)

    def test_no_activity_policy_needs_a_verified_reboot_and_one_member_adapter(self):
        fixture = MainRebootFixture()
        reboot = fixture.reboot()
        with self.assertRaisesRegex(RuntimeError, "Verify the main reboot"):
            reboot.assert_no_activity_policy(fixture.screen, fixture.player)
        fixture, reboot = self.rebooted()
        for front, source in ((fixture.avr, fixture.player), (fixture.tv, fixture.player),
                              (fixture.player, fixture.screen)):
            with self.subTest(front=front.name, source=source.name):
                with self.assertRaisesRegex(ValueError, "one member adapter"):
                    reboot.assert_no_activity_policy(front, source)


if __name__ == "__main__":
    unittest.main()
