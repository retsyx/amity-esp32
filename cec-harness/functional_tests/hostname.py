"""Hostname changes retain local and distributed activity control across boots."""

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.hostname import HostnameChanges
from amity_harness.trace import event


def _equipment(dut, label):
    tentacle = dut.tentacle(0)
    tv = TvComponent(tentacle.front, f"{label} TV")
    avr = AvrComponent(tentacle.back, f"{label} AVR")
    return tv, avr, PlaybackComponent(avr, f"{label} Player", port=1)


def _scan(main, components):
    scan = main.scan_hdmi()
    scan.assert_contains(*components)
    scan.assert_contains_only(*components)
    for component in components:
        scan.assert_on(component, component.endpoint)


def _exercise(main, activities, systems, *, phase):
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
        event("hostname.activity_verified", dut=main.name, owner=active[0].dut.identity.device_id,
              activity_id=activity.activity_id, phase=phase)


def test_hostname_local():
    main = DUT("amity-dut")
    system = _equipment(main, "Name")
    with TestBed(main, *system, preserve_dut_on_failure=True):
        _scan(main, system)
        tv, avr, player = system
        activity = main.create_activity("Hostname local", display=tv, audio=avr, source=player)
        with HostnameChanges(main, activities=(activity,)) as names:
            names.canonicalize()
            _exercise(main, (activity,), (system,), phase="before")
            renamed = main.name + "-t"
            names.rename(main, renamed)
            names.assert_hostname(main, renamed)
            _exercise(main, (activity,), (system,), phase="renamed")
            names.reboot(main)
            names.assert_hostname(main, renamed)
            _exercise(main, (activity,), (system,), phase="rebooted")
            names.restore()


def test_hostname_consortium():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    systems = (_equipment(main, "Main"), _equipment(secondary, "Member"))
    components = tuple(component for system in systems for component in system)
    with TestBed(main, secondary, *components, members=(secondary,), preserve_dut_on_failure=True):
        member = main.add_amity(secondary)
        member.assert_connected()
        _scan(main, components)
        activities = tuple(main.create_activity(
            f"Hostname {owner.name}", display=tv, audio=avr, source=player)
            for owner, (tv, avr, player) in zip((main, secondary), systems))
        with HostnameChanges(main, members=(member,), activities=activities) as names:
            names.canonicalize()
            cases = ((main, (main,)), (main, (secondary,)),
                     (secondary, (main,)), (secondary, (secondary,)),
                     (main, (main, secondary)), (secondary, (main, secondary)))
            for number, (renamed_owner, rebooted) in enumerate(cases, start=1):
                phase = f"case-{number}"
                _exercise(main, activities, systems, phase=phase + "-before")
                renamed = renamed_owner.name + "-t"
                names.rename(renamed_owner, renamed)
                names.assert_hostname(renamed_owner, renamed)
                _exercise(main, activities, systems, phase=phase + "-renamed")
                names.reboot(*rebooted)
                names.assert_hostname(renamed_owner, renamed)
                _exercise(main, activities, systems, phase=phase + "-rebooted")
                names.restore()
