"""Identical equipment identities remain scoped to their physical location."""

from contextlib import ExitStack

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent,
                           Key, PowerState)
from amity_harness.qualified_names import observe_no_control


def same_name_equipment(dut, tentacle=0):
    connection = dut.tentacle(tentacle)
    tv = TvComponent(connection.front, "Same TV")
    avr = AvrComponent(connection.back, "Same AVR")
    player = PlaybackComponent(avr, "Same Player", port=1)
    return tv, avr, player


def exercise_qualified_activities(main, systems, names):
    components = tuple(component for system in systems for component in system)
    scan = main.scan_hdmi()
    scan.assert_contains(*components)
    scan.assert_contains_only(*components)
    for component in components:
        scan.assert_on(component, component.endpoint)

    with ExitStack() as stack:
        activities = [stack.enter_context(main.create_activity(
            name, display=system[0], audio=system[1], source=system[2]))
            for name, system in zip(names, systems, strict=True)]
        for index, (activity, active) in enumerate(zip(activities, systems)):
            inactive = tuple(component for other, system in enumerate(systems)
                             if other != index for component in system)
            for component in components:
                component.assert_power(PowerState.STANDBY)
            with observe_no_control(scan, *inactive) as observation:
                started = activity.start()
                for component in active:
                    component.wait_for_power(PowerState.ON, since=started)
                active[1].wait_for_input(active[2], since=started)
                main.press_key(Key.VOLUME_UP).assert_received_by(active[1])
                main.press_key(Key.RIGHT).assert_received_by(active[2])
                activity.assert_active()
                for component in inactive:
                    component.assert_power(PowerState.STANDBY)
                observation.assert_clear()
            stopped = activity.standby()
            for component in active:
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
            for component in components:
                component.assert_power(PowerState.STANDBY)


def test_qualified_names():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    first, second = same_name_equipment(main), same_name_equipment(secondary)
    components = (*first, *second)
    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            exercise_qualified_activities(main, (first, second),
                                          ("Qualified main", "Qualified member"))
            member.assert_connected()
