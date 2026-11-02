"""Identical equipment identities remain scoped to their physical Amity owner."""

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent,
                           Key, PowerState)
from amity_harness.qualified_names import observe_no_control


def _equipment(dut):
    connection = dut.tentacle(0)
    tv = TvComponent(connection.front, "Same TV")
    avr = AvrComponent(connection.back, "Same AVR")
    player = PlaybackComponent(avr, "Same Player", port=1)
    return tv, avr, player


def test_qualified_names():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    first, second = _equipment(main), _equipment(secondary)
    components = (*first, *second)
    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains(*components)
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)

            with main.create_activity("Qualified main", display=first[0], audio=first[1], source=first[2]) as local:
                with main.create_activity("Qualified member", display=second[0], audio=second[1], source=second[2]) as remote:
                    for activity, active, inactive in ((local, first, second), (remote, second, first)):
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
            member.assert_connected()
