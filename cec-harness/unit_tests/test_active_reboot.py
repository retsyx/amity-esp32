"""A member reboot preserves the current activity and its physical effects."""

from copy import deepcopy
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, CecAddress, DUT, Frame, Hooks, PlaybackComponent, TvComponent
from amity_harness.components import PowerState
from amity_harness.config import RunConfig, use_config
from amity_harness.reboot import ActiveMemberReboot


class Clock:
    def __init__(self):
        self.now = 100.0

    def monotonic(self):
        return self.now

    def monotonic_ns(self):
        return int(self.now * 1e9)

    def sleep(self, seconds):
        self.now += seconds


class RebootFixture:
    def __init__(self):
        self.clock = Clock()
        with use_config(RunConfig(insecure=True, timeout=1, cec_timeout=0.01)):
            self.main, self.secondary, self.other = (DUT(name) for name in
                ("amity-dut", "amity-dut-2", "amity-dut-3"))
        self.owners = (self.main, self.secondary, self.other)
        self.boots = {dut.identity.device_id: str(index + 1) * 32
                      for index, dut in enumerate(self.owners)}
        self.group = "4" * 32
        self.events, self.callbacks, self.requests = [], [], []
        self.on_usb = lambda: None
        self.skip_reboot = False
        self.activity_current, self.activity_settling, self.activity_error = 0, False, ""
        self.config = {
            "version": 3, "consortium_id": self.group,
            "controller_octopus_id": self.main.identity.device_id,
            "config_revision": "7", "presentation_revision": "8",
            "octopuses": [{"octopus_id": dut.identity.device_id, "supported_tentacles": 2}
                          for dut in self.owners],
            "activities": [{"activity_id": "9" * 32, "label": "Watch"}],
            "components": [{"component_id": "a" * 32, "label": "Player"}],
        }
        self.settings_changes = {}
        self.tv = TvComponent(self.main.tentacle(0).front, "TV", address=CecAddress(0, 0), power="on")
        self.avr = AvrComponent(self.secondary.tentacle(0).back, "AVR",
                                address=CecAddress(5, 0), power="on")
        self.player = PlaybackComponent(self.avr, "Player", port=1,
                                        address=CecAddress(4, 0x1000), power="on")
        self.components = (self.tv, self.avr, self.player)
        self.avr.active_input = self.player.address.physical
        self.session = SimpleNamespace(components=self.components, _lock=threading.RLock(),
            _stop=threading.Event(), _record=lambda *args, **kwargs: None,
            power_changed=lambda *args, **kwargs: None,
            assert_healthy=lambda: None,
            _wire_idle_since_ns={component.gpio: 10**30 for component in self.components})
        self.bed = SimpleNamespace(_entered=True, _closed=False, session=self.session,
            components=self.components, duts=self.owners, members=self.owners[1:], _member_proofs={},
            add_cleanup=self.callbacks.append, control_observations=[])
        for dut in self.owners:
            dut._testbed = self.bed
        for component in self.components:
            component._session = self.session
        self.members = tuple(self.member(dut) for dut in self.owners[1:])
        self.member_proof = self.members[0]
        self.members[1]._initial_system["membership"]["peers"] = [
            {"octopus_id": self.secondary.identity.device_id,
             "boot_id": self.boots[self.secondary.identity.device_id]}]
        self.main.request = self.request
        self.main.authenticate = lambda: None
        self.main.scan_hdmi = self.scan
        self.secondary.usb_reboot = self.usb_reboot
        self.main.last_scan = self.make_scan()
        self.activity = SimpleNamespace(dut=self.main, session=self.session, _testbed=self.bed,
            _created=True, _closed=False, activity_id="9" * 32, name="Watch",
            _created_row={"activity_id": "9" * 32, "name": "Watch"},
            boot=self.boots[self.main.identity.device_id], _saved_group=self.group,
            components={"display": self.tv, "audio": self.avr, "source": self.player, "switch": None},
            scan=self.main.last_scan, assert_active=self.assert_active, refresh_scan=self.refresh_scan)

    def member(self, dut):
        proof = SimpleNamespace(main=self.main, candidate=dut, name=dut.name, session=self.session,
            device_id=dut.identity.device_id, _created=True, _closed=False,
            boot=self.boots[self.main.identity.device_id], member_boot=self.boots[dut.identity.device_id],
            group=self.group, _initial_system={"boot_id": self.boots[self.main.identity.device_id],
                "membership": {"peers": []}})
        self.bed._member_proofs[proof.device_id] = proof
        return proof

    def system(self):
        peers = [{"octopus_id": dut.identity.device_id, "hostname": dut.name,
                  "boot_id": self.boots[dut.identity.device_id], "enrolled": True,
                  "ready": True, "supported_tentacles": 2} for dut in self.owners[1:]]
        return {"octopus_id": self.main.identity.device_id, "consortium_id": self.group,
            "boot_id": self.boots[self.main.identity.device_id], "hostname": self.main.name,
            "mode": "coordinator", "ready": True, "configuration_pending": False,
            "applied_config_revision": self.config["config_revision"],
            "applied_presentation_revision": self.config["presentation_revision"],
            "named_devices": [{"octopus_id": dut.identity.device_id, "hostname": dut.name}
                              for dut in self.owners],
            "membership": {"coordinator": True, "member": False, "authority_ready": True,
                           "peers": peers}}

    def request(self, path, data=None, **kwargs):
        self.requests.append((path, deepcopy(data)))
        if data is not None:
            raise AssertionError("An active reboot must not mutate the shipping activity or settings")
        if path == "/api/system":
            return self.system()
        if path == "/api/config":
            return deepcopy(self.config)
        if path == "/api/activity-editor":
            return {"activities": [deepcopy(self.activity._created_row)]}
        if path == "/api/activity-editor/status":
            return {"expected_boot_id": self.boots[self.main.identity.device_id],
                "current_activity": self.activity_current, "activity_settling": self.activity_settling,
                "activity_error": self.activity_error}
        if path.startswith("/api/devices/") and path.endswith("/settings"):
            owner = path.split("/")[3]
            dut = next(dut for dut in self.owners if dut.identity.device_id == owner)
            settings = {"boot_id": self.boots[owner], "hostname": dut.name,
                "hostname_revision": "1", "settings_revision": "2",
                "adapters": [{"tentacle_id": index, "configured": True,
                    "default_front_physical_address": "1.0.0.0"} for index in range(2)]}
            settings.update(deepcopy(self.settings_changes.get(owner, {})))
            return {"status": "ready", "error": "", "settings": settings}
        raise AssertionError("Unexpected shipping endpoint: " + path)

    def assert_active(self):
        self.events.append(("assert_active", self.activity_current))
        self.activity.scan.validate_owners(self.components)
        state = self.request("/api/activity-editor/status")
        if (state["expected_boot_id"] != self.activity.boot or state["current_activity"] != 0
                or state["activity_settling"] is not False or state["activity_error"] != ""):
            raise AssertionError("The exact saved activity is not stably current")
        return self.activity

    def make_scan(self):
        fixture = self

        class Scan:
            def __init__(self):
                self.dut, self._session, self._testbed = fixture.main, fixture.session, fixture.bed
                self._boot = fixture.boots[fixture.main.identity.device_id]
                self._group = fixture.group
                self.expected = fixture.components
                self.started = fixture.clock.monotonic_ns()
                self.owner_duts = {dut.identity.device_id: dut for dut in fixture.owners}
                self.owner_boots = dict(fixture.boots)
                self.initiators = {fixture.tv.gpio: 4, fixture.avr.gpio: 0}

            def validate_owners(self, components):
                if self.owner_boots != fixture.boots or any(
                        proof.member_boot != fixture.boots[proof.device_id] for proof in fixture.members):
                    raise AssertionError("Stale owner boot evidence")

            def assert_contains(self, *components):
                if components != fixture.components:
                    raise AssertionError("The fresh scan lost fixture equipment")

            assert_contains_only = assert_contains

        return Scan()

    def scan(self, **kwargs):
        self.events.append(("scan",))
        self.main.last_scan = self.make_scan()
        self.main.last_scan.validate_owners(self.components)
        return self.main.last_scan

    def refresh_scan(self, scan):
        self.events.append(("refresh_scan",))
        scan.validate_owners(self.components)
        self.activity.scan = scan

    def usb_reboot(self):
        self.events.append(("usb_reboot",))
        self.clock.sleep(0.01)
        if not self.skip_reboot:
            self.boots[self.secondary.identity.device_id] = "b" * 32
        self.on_usb()
        return "/dev/ttyACM-test"

    def receive(self, component, opcode, operands=b"", *, source=8, destination=None, transport="wire"):
        self.clock.sleep(0.001)
        frame = Frame(source, component.address.logical if destination is None else destination,
            opcode, operands, timestamp_ns=self.clock.monotonic_ns())
        component.receive(frame, transport=transport)


class ActiveMemberRebootTests(unittest.TestCase):
    def fixture(self):
        fixture = RebootFixture()
        timer = patch("amity_harness.reboot.time", fixture.clock)
        timer.start()
        self.addCleanup(timer.stop)
        return fixture

    def test_reboot_once_preserves_activity_and_refreshes_only_the_proven_member_boot(self):
        fixture = self.fixture()
        before = dict(fixture.boots)
        old_scan = fixture.activity.scan

        def still_fenced_during_reset():
            self.assertEqual(fixture.member_proof.member_boot, before[fixture.member_proof.device_id])
            self.assertEqual(old_scan.owner_boots, before)

        fixture.on_usb = still_fenced_during_reset
        with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
            self.assertIs(reboot.reboot(), reboot)
            self.assertIs(reboot.assert_continued(), reboot)
            with self.assertRaisesRegex(RuntimeError, "exactly one"):
                reboot.reboot()
        self.assertEqual(fixture.events.count(("usb_reboot",)), 1)
        self.assertEqual(fixture.events.count(("scan",)), 1)
        self.assertEqual(fixture.events.count(("refresh_scan",)), 1)
        self.assertGreater(fixture.events.count(("assert_active", 0)), 1)
        self.assertEqual(fixture.member_proof.member_boot, "b" * 32)
        self.assertEqual(fixture.members[1].member_boot, before[fixture.other.identity.device_id])
        self.assertEqual(fixture.activity.boot, before[fixture.main.identity.device_id])
        self.assertEqual(fixture.members[1]._initial_system["membership"]["peers"][0]["boot_id"], "b" * 32)
        self.assertIsNot(fixture.activity.scan, old_scan)
        self.assertFalse(any(body is not None for _, body in fixture.requests))
        self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_main_activity_must_remain_current_settled_and_error_free_during_recovery(self):
        for field, value in (("activity_current", -1), ("activity_settling", True),
                             ("activity_error", "execution failed")):
            with self.subTest(field=field):
                fixture = self.fixture()
                fixture.on_usb = lambda: setattr(fixture, field, value)
                with self.assertRaisesRegex(AssertionError, "activity changed or stopped"):
                    with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                        reboot.reboot()
                self.assertNotIn(("scan",), fixture.events)
                self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_only_a_changed_target_boot_can_refresh_scan_and_owner_proof(self):
        for changed in (None, "main", "other"):
            with self.subTest(changed=changed):
                fixture = self.fixture()
                original = fixture.member_proof.member_boot
                if changed is None:
                    fixture.skip_reboot = True
                else:
                    fixture.on_usb = lambda: fixture.boots.__setitem__(
                        getattr(fixture, changed).identity.device_id, "c" * 32)
                with self.assertRaises((AssertionError, TimeoutError)):
                    with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                        reboot.reboot()
                self.assertEqual(fixture.member_proof.member_boot, original)
                self.assertNotIn(("scan",), fixture.events)
                self.assertNotIn(("refresh_scan",), fixture.events)

    def test_saved_configuration_and_adapter_settings_must_survive_before_rebinding(self):
        for changed in ("configuration", "settings"):
            with self.subTest(changed=changed):
                fixture = self.fixture()
                original = fixture.member_proof.member_boot

                def mutate_saved_state():
                    if changed == "configuration":
                        fixture.config["components"][0]["label"] = "Changed equipment"
                    else:
                        fixture.settings_changes[fixture.secondary.identity.device_id] = {
                            "adapters": [{"tentacle_id": 0, "configured": False,
                                          "default_front_physical_address": "2.0.0.0"}]}

                fixture.on_usb = mutate_saved_state
                with self.assertRaisesRegex(AssertionError, "changed saved"):
                    with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                        reboot.reboot()
                self.assertEqual(fixture.member_proof.member_boot, original)
                self.assertNotIn(("scan",), fixture.events)

    def test_transient_power_commands_fail_even_after_recovery_or_fault_hook_suppression(self):
        cases = ((0x36, b"", 0, None), (0x36, b"", 15, "drop"),
                 (0x44, b"\x6c", 8, "drop"), (0x44, b"\x40", 8, "rewrite"),
                 (0x44, b"\x6b", 8, None))
        for opcode, operands, source, hook in cases:
            with self.subTest(opcode=opcode, operands=operands, source=source, hook=hook):
                fixture = self.fixture()
                if hook:
                    fixture.avr.hooks = Hooks(before_rx=lambda component, frame:
                        None if hook == "drop" else Frame(frame.source, frame.destination, 0x45,
                                                          timestamp_ns=frame.timestamp_ns))

                def transient_command():
                    fixture.receive(fixture.avr, opcode, operands, source=source)
                    fixture.receive(fixture.avr, 0x44, b"\x6d", source=source)

                fixture.on_usb = transient_command
                with self.assertRaisesRegex(AssertionError, "interrupted power, audio or input"):
                    with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                        reboot.reboot()
                self.assertEqual(fixture.avr.power, PowerState.ON)
                self.assertNotIn(("scan",), fixture.events)
                self.assertFalse(fixture.avr._wire_observers)

    def test_transient_routing_or_audio_loss_fails_even_after_the_final_state_is_restored(self):
        for opcode in (0x82, 0x86, 0x70):
            with self.subTest(opcode=opcode):
                fixture = self.fixture()
                fixture.avr.system_audio = True

                def reroute_and_recover():
                    if opcode == 0x70:
                        # Suppress reply transmission without hiding original
                        # physical evidence from the continuity observer.
                        fixture.avr.hooks = Hooks(before_rx=lambda component, frame: None)
                        fixture.receive(fixture.avr, opcode)
                        fixture.avr.system_audio = True
                    else:
                        fixture.receive(fixture.avr, opcode, b"\x20\x00", destination=15)
                        fixture.receive(fixture.avr, opcode, b"\x10\x00", destination=15)

                fixture.on_usb = reroute_and_recover
                with self.assertRaisesRegex(AssertionError, "interrupted power, audio or input"):
                    with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                        reboot.reboot()
                self.assertEqual(fixture.avr.active_input, fixture.player.address.physical)
                self.assertTrue(fixture.avr.system_audio)
                self.assertNotIn(("scan",), fixture.events)

    def test_undrained_wire_cannot_establish_continuity_or_refresh_evidence(self):
        fixture = self.fixture()
        fixture.session._wire_idle_since_ns = {}
        original = fixture.member_proof.member_boot
        with self.assertRaises(TimeoutError):
            with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                with patch.object(reboot._condition, "wait", side_effect=fixture.clock.sleep):
                    reboot.reboot()
        self.assertEqual(fixture.member_proof.member_boot, original)
        self.assertNotIn(("scan",), fixture.events)
        self.assertTrue(all(not component._wire_observers for component in fixture.components))

    def test_physical_evidence_requires_a_valid_wire_timestamp(self):
        fixture = self.fixture()

        def invalid_timestamp():
            fixture.avr.receive(Frame(8, 5, 0x36, timestamp_ns=0))
            fixture.avr.power_on()

        fixture.on_usb = invalid_timestamp
        with self.assertRaisesRegex(AssertionError, "timestamped physical"):
            with ActiveMemberReboot(fixture.activity, fixture.member_proof) as reboot:
                reboot.reboot()
        self.assertNotIn(("scan",), fixture.events)
        self.assertTrue(all(not component._wire_observers for component in fixture.components))


if __name__ == "__main__":
    unittest.main()
