"""Activity control and source-theft suppression across Amity owners."""

from contextlib import ExitStack

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState


def _equipment(dut, name):
    tentacle = dut.tentacle(0)
    tv = TvComponent(tentacle.front, f"{name} TV")
    avr = AvrComponent(tentacle.back, f"{name} AVR")
    player = PlaybackComponent(avr, f"{name} Player", port=1)
    return tv, avr, player


def test_consortium():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    first, second = _equipment(main, "Main"), _equipment(secondary, "Member")
    components = (*first, *second)
    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains(*components)
            scan.assert_contains_only(*components)
            for dut, (tv, avr, player) in ((main, first), (secondary, second)):
                scan.assert_on(tv, dut.tentacle(0).front)
                scan.assert_on(avr, dut.tentacle(0).back)
                scan.assert_on(player, dut.tentacle(0).back)

            with main.create_activity("Main system", display=first[0], audio=first[1], source=first[2]) as local:
                with main.create_activity("Member system", display=second[0], audio=second[1], source=second[2]) as remote:
                    for activity, active, inactive in ((local, first, second), (remote, second, first)):
                        active_avr, active_player = active[1:]
                        thief = inactive[2]
                        for component in components:
                            component.assert_power(PowerState.STANDBY)
                        started = activity.start()
                        for component in active:
                            component.wait_for_power(PowerState.ON, since=started)
                        for component in inactive:
                            component.assert_power(PowerState.STANDBY)
                        active_avr.wait_for_input(active_player, since=started)
                        activity.assert_active()

                        announcement = thief.activate()
                        assert announcement.ok
                        thief.wait_for_standby_after(announcement, within=2)
                        assert active_avr.active_input == active_player.address.physical
                        for component in active:
                            component.assert_power(PowerState.ON)
                        for component in inactive:
                            component.assert_power(PowerState.STANDBY)
                        assert active_player.active and not thief.active
                        activity.assert_active()

                        stopped = activity.standby()
                        for component in active:
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        for component in components:
                            component.assert_power(PowerState.STANDBY)
            member.assert_connected()


def test_consortium_three_duts():
    main, secondary, tertiary = DUT("amity-dut"), DUT("amity-dut-2"), DUT("amity-dut-3")
    owners = (main, secondary, tertiary)
    systems = (_equipment(main, "Main"), _equipment(secondary, "Member2"),
               _equipment(tertiary, "Member3"))
    components = tuple(component for system in systems for component in system)
    with TestBed(*owners, *components, members=(secondary, tertiary)):
        with main.add_amity(secondary) as second_member:
            second_member.assert_connected()
            with main.add_amity(tertiary) as third_member:
                third_member.assert_connected()
                scan = main.scan_hdmi()
                scan.assert_contains(*components)
                scan.assert_contains_only(*components)
                for dut, (tv, avr, player) in zip(owners, systems):
                    scan.assert_on(tv, dut.tentacle(0).front)
                    scan.assert_on(avr, dut.tentacle(0).back)
                    scan.assert_on(player, dut.tentacle(0).back)

                with ExitStack() as activities:
                    saved = [activities.enter_context(main.create_activity(
                        name, display=tv, audio=avr, source=player))
                        for name, (tv, avr, player) in zip(
                            ("Main system", "Member 2 system", "Member 3 system"), systems)]
                    for activity, active in zip(saved, systems):
                        inactive = tuple(system for system in systems if system is not active)
                        active_avr, active_player = active[1:]
                        for component in components:
                            component.assert_power(PowerState.STANDBY)
                        started = activity.start()
                        for component in active:
                            component.wait_for_power(PowerState.ON, since=started)
                        for system in inactive:
                            for component in system:
                                component.assert_power(PowerState.STANDBY)
                        active_avr.wait_for_input(active_player, since=started)
                        activity.assert_active()

                        for system in inactive:
                            thief = system[2]
                            announcement = thief.activate()
                            assert announcement.ok
                            thief.wait_for_standby_after(announcement, within=2)
                            assert active_avr.active_input == active_player.address.physical
                            for component in active:
                                component.assert_power(PowerState.ON)
                            for other in inactive:
                                for component in other:
                                    component.assert_power(PowerState.STANDBY)
                            assert active_player.active and all(not other[2].active for other in inactive)
                            activity.assert_active()

                        stopped = activity.standby()
                        for component in active:
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        for component in components:
                            component.assert_power(PowerState.STANDBY)
                third_member.assert_connected()
            second_member.assert_connected()
