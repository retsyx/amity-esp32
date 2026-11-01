"""Two CEC displays sharing one AVR with only one CEC-capable output."""

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState


def test_dual_display():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-4")
    tv_connection, projector_connection = secondary.tentacle(1), secondary.tentacle(2)
    tv = TvComponent(tv_connection.front, "Dual TV")
    avr = AvrComponent(tv_connection.back, "Dual AVR")
    player = PlaybackComponent(avr, "Dual Player", port=1)
    projector = TvComponent(projector_connection.front, "Dual Projector")
    # The AVR's second output attaches to projector_connection.back without
    # CEC support: no AVR/player responder or CEC bridge belongs on that wire.
    components = (tv, avr, player, projector)

    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains(*components)
            scan.assert_contains_only(*components)
            scan.assert_on(tv, tv_connection.front)
            scan.assert_on(avr, tv_connection.back)
            scan.assert_on(player, tv_connection.back)
            scan.assert_on(projector, projector_connection.front)

            with main.create_activity("Dual display TV", display=tv, audio=avr, source=player) as tv_activity:
                with main.create_activity("Dual display projector", display=projector, audio=avr,
                                          source=player) as projector_activity:
                    for first, display, second, other in (
                            (tv_activity, tv, projector_activity, projector),
                            (projector_activity, projector, tv_activity, tv)):
                        for component in components:
                            component.assert_power(PowerState.STANDBY)

                        started = first.start()
                        for component in (display, avr, player):
                            component.wait_for_power(PowerState.ON, since=started)
                        other.assert_power(PowerState.STANDBY)
                        avr.wait_for_input(player, since=started)
                        first.assert_active()

                        # Switch directly: shared source/audio stay powered,
                        # while the previously selected display must turn off.
                        switched = second.start()
                        display.wait_for_power(PowerState.STANDBY, since=switched,
                                               command="standby", directed=True)
                        other.wait_for_power(PowerState.ON, since=switched)
                        for component in (avr, player):
                            component.assert_power(PowerState.ON)
                        avr.wait_for_input(player, since=switched)
                        second.assert_active()

                        stopped = second.standby()
                        for component in (other, avr, player):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        for component in components:
                            component.assert_power(PowerState.STANDBY)
            member.assert_connected()
