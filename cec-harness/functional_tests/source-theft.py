"""Stop an isolated source thief before its announcement reaches the AVR."""

from amity_harness import (
    DUT, TestBed, TvComponent, AvrComponent, AvrAttachment, PlaybackComponent,
    CecAddress, PowerState,
)


def test_source_theft_isolated():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-4")
    output, source_connection = secondary.tentacle(1), secondary.tentacle(2)
    tv = TvComponent(output.front, "Isolated TV")
    avr = AvrComponent(output.back, "Isolated AVR")
    player = PlaybackComponent(avr, "Media Player", port=1)
    # The AVR's input 2 attaches to the second tentacle's front. This is the
    # same AVR on a second wire, with shared state and separate wire evidence.
    avr_input = AvrAttachment(avr, source_connection.front)
    thief = PlaybackComponent(source_connection.back, "Source Thief",
                              address=CecAddress(4, "1.2.0.0"))
    observations = (tv, avr, player, avr_input, thief)
    equipment = (tv, avr, player, thief)

    with TestBed(main, secondary, *observations, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            # No EDID fixture: use the ordinary UI setting for AVR input 2.
            # Restore it after activities close and before removing the member.
            with member.front_physical_address(2, "1.2.0.0"):
                scan = main.scan_hdmi()
                scan.assert_contains(*observations)
                scan.assert_contains_only(*observations)
                scan.assert_on(tv, output.front)
                scan.assert_on(avr, output.back)
                scan.assert_on(player, output.back)
                scan.assert_on(avr_input, source_connection.front)
                scan.assert_on(thief, source_connection.back)

                with main.create_activity("Isolated thief", display=tv, audio=avr, source=thief) as thief_activity:
                    with main.create_activity("Protected player", display=tv, audio=avr, source=player) as activity:
                        for component in equipment:
                            component.assert_power(PowerState.STANDBY)
                        started = activity.start()
                        for component in (tv, avr, player):
                            component.wait_for_power(PowerState.ON, since=started)
                        thief.assert_power(PowerState.STANDBY)
                        avr.wait_for_input(player, since=started)
                        activity.assert_active()

                        # Observe the upstream AVR input as well as its output:
                        # a leaked announcement fails even if the route recovers.
                        for _ in range(3):
                            with avr.observe_source_isolation(thief, upstream=avr_input, since=started) as watch:
                                announcement = thief.activate()
                                assert announcement.ok
                                thief.wait_for_standby_after(announcement, within=0.250)
                                watch.assert_suppressed(announcement)
                            assert avr.active_input == player.address.physical
                            assert player.active and not thief.active
                            for component in (tv, avr, player):
                                component.assert_power(PowerState.ON)
                            activity.assert_active()

                        stopped = activity.standby()
                        for component in (tv, avr, player):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        for component in equipment:
                            component.assert_power(PowerState.STANDBY)

                        selected = thief_activity.start()
                        for component in (tv, avr, thief):
                            component.wait_for_power(PowerState.ON, since=selected)
                        player.assert_power(PowerState.STANDBY)
                        # Positive control: legitimate use of this source must
                        # select the AVR input through the second front wire.
                        avr_input.wait_for_active_source(thief, since=selected)
                        assert avr.active_input == thief.address.physical
                        for component in (tv, avr, thief):
                            component.assert_power(PowerState.ON)
                        player.assert_power(PowerState.STANDBY)
                        assert thief.active
                        thief_activity.assert_active()
                        stopped = thief_activity.standby()
                        for component in (tv, avr, thief):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        for component in equipment:
                            component.assert_power(PowerState.STANDBY)
            member.assert_connected()
