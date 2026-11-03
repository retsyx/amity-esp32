"""Optional all-board firmware prelude with real activities on every owner."""

from copy import deepcopy

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.config import current_config
from amity_harness.firmware import FirmwareBundle, FirmwareUpgrade, saved_configuration, saved_device_settings
from amity_harness.trace import event


def _equipment(dut):
    # Resolve a wired pair from the authoritative bench table, never a GPIO map
    # or an assumption about how many connectors a board supports.
    index = min(index for name, index, side in dut.wiring.cec if name == dut.name and side == "front")
    connection = dut.tentacle(index)
    label = "U" + dut.identity.device_id[-6:]
    tv = TvComponent(connection.front, f"{label} TV")
    avr = AvrComponent(connection.back, f"{label} AVR")
    return tv, avr, PlaybackComponent(avr, f"{label} Player", port=1)


def _scan(main, components):
    scan = main.scan_hdmi()
    scan.assert_contains(*components)
    scan.assert_contains_only(*components)
    for component in components:
        scan.assert_on(component, component.endpoint)
    return scan


def _exercise(main, activities, systems, *, phase):
    """State alone is insufficient: every assertion needs new wire commands."""
    components = tuple(component for system in systems for component in system)
    for activity, active in zip(activities, systems):
        for component in components:
            component.assert_power(PowerState.STANDBY)
        started = activity.start()
        for component in active:
            component.wait_for_power(PowerState.ON, since=started)
        active[1].wait_for_input(active[2], since=started)
        activity.assert_active()
        main.press_key(Key.VOLUME_UP).assert_received_by(active[1])
        main.press_key(Key.RIGHT).assert_received_by(active[2])
        for component in components:
            if component not in active:
                component.assert_power(PowerState.STANDBY)
        stopped = activity.standby()
        for component in active:
            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
        for component in components:
            component.assert_power(PowerState.STANDBY)
        event("firmware.activity_verified", dut=main.name, owner=active[0].dut.identity.device_id,
              activity_id=activity.activity_id, phase=phase)


class _CleanBench:
    """Check the ordinary helpers' independent cleanup after all callbacks."""

    _cleanup_priority = 3
    _cleanup_retryable = True

    def __init__(self, main, members, baseline, initial_system):
        self.main, self.members = main, members
        self.baseline, self.initial_system = deepcopy(baseline), deepcopy(initial_system)
        response = main.request("/api/devices/" + main.identity.device_id + "/settings")
        body = response.get("settings") or {}
        if response.get("status") != "ready" or response.get("error") or body.get("boot_id") != initial_system["boot_id"]:
            raise AssertionError("The main's saved settings are not ready before the firmware test")
        self.settings = saved_device_settings(body)
        self.done = False
        main._testbed.add_cleanup(self.close)

    def close(self):
        if self.done:
            return
        system, config, state = self.main.get_system(), self.main.request("/api/config"), self.main.get_state()
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.main.identity.device_id
                or system.get("consortium_id") != self.initial_system["consortium_id"]
                or membership.get("coordinator") is not True or membership.get("authority_ready") is not True
                or membership.get("member") is not False or membership.get("peers") != []
                or system.get("ready") is not True
                or system.get("configuration_pending") is not False
                or state.get("expected_boot_id") != system.get("boot_id")
                or state.get("current_activity") != -1 or state.get("activity_settling") is not False
                or system.get("applied_config_revision") != config.get("config_revision")
                or system.get("applied_presentation_revision") != config.get("presentation_revision")
                or system.get("named_devices") != self.initial_system.get("named_devices")):
            raise AssertionError("Firmware prelude did not leave the original main clean and idle")
        before, after = saved_configuration(self.baseline), saved_configuration(config)
        old_slots, new_slots = before.pop("external_slots", []), after.pop("external_slots", [])
        if before != after:
            raise AssertionError("Firmware prelude left temporary activities, equipment or Amity settings")
        # Activity deletion may retire its external-home slot. Accept only the
        # existing framework's exact test-owned tombstone rule, never restore
        # an old full configuration over another resource's work.
        if new_slots != old_slots and (not self.members or not self.members[0]._owned_retired_slots(old_slots, new_slots)):
            raise AssertionError("Firmware prelude left unrelated external activity slots")
        response = self.main.request("/api/devices/" + self.main.identity.device_id + "/settings")
        body = response.get("settings") or {}
        if (response.get("status") != "ready" or response.get("error") or body.get("boot_id") != system["boot_id"]
                or saved_device_settings(body) != self.settings):
            raise AssertionError("Firmware prelude changed the main's saved adapter settings or hostname")
        for member in self.members:
            records = [row for row in self.main.membership_operations
                       if row.get("device_id") == member.device_id and row.get("phase") == "cleanup"]
            if (not member._closed or not records or records[-1].get("reset") != "scheduled"
                    or records[-1].get("retained_member") is not False):
                raise AssertionError("A secondary did not acknowledge its scheduled factory reset")
            settings = self.main.request("/api/devices/" + member.device_id + "/settings")
            if settings.get("status") != "offline" or settings.get("settings") is not None:
                raise AssertionError("A removed secondary still has saved UI settings")
        self.done = True
        event("firmware.cleanup_verified", dut=self.main.name, reset_scheduled=[member.device_id for member in self.members])


def test_firmware_upgrade():
    runtime = current_config()
    if runtime.firmware is None:
        raise ValueError("Select an Amity update bundle with --firmware")
    # Validate the local file before touching the bench. Publisher/native OTA
    # admission remains the firmware's responsibility, through the real UI.
    bundle = FirmwareBundle.load(runtime.firmware)
    main = DUT("amity-dut")
    secondaries = tuple(DUT(name, wiring=main.wiring) for name in main.wiring.identities if name != main.name)
    if not secondaries:
        raise ValueError("The firmware prelude requires secondary Amities in TEST-HARNESS.md")
    owners = (main, *secondaries)
    systems = tuple(_equipment(dut) for dut in owners)
    components = tuple(component for system in systems for component in system)
    with TestBed(*owners, *components, members=secondaries):
        baseline, initial = main.request("/api/config"), main.get_system()
        state = main.get_state()
        if (baseline.get("activities") != [] or baseline.get("components") != []
                or initial.get("membership", {}).get("peers") != []
                or state.get("current_activity") != -1 or state.get("activity_settling") is not False):
            raise AssertionError("Start the firmware prelude with an idle main and no saved activities, equipment or members")
        if main.request("/api/setup").get("image_elf_sha256") == bundle.image_elf_sha256:
            raise ValueError("The upgrade test requires a different firmware image")
        members, activities = [], []
        _CleanBench(main, members, baseline, initial)
        for secondary in secondaries:
            members.append(main.add_amity(secondary, timeout=max(runtime.timeout, runtime.firmware_timeout)))
        _scan(main, components)
        for owner, (tv, avr, player) in zip(owners, systems):
            activities.append(main.create_activity(f"Upgrade {owner.name}", display=tv, audio=avr, source=player))
        _exercise(main, activities, systems, phase="before")
        upgrade = FirmwareUpgrade(main, members, bundle, activities=activities, timeout=runtime.firmware_timeout)
        upgrade.run()
        upgrade.rebind_members()
        # The upgrade verified the complete roster and every settings snapshot.
        # Each Add helper deliberately retains only its original prior roster;
        # its single-add assertion must not absorb later additions as baseline.
        scan = _scan(main, components)
        upgrade.rebind_activities(activities, scan)
        _exercise(main, activities, systems, phase="after")
        # TestBed owns cleanup in dependency order even on assertion failures:
        # reconcile OTA boots, delete activities/equipment, remove/reset every
        # member, verify the clean main, release CEC addresses, then log out.
