"""Member-owned playback continues through that member's USB reboot."""

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.reboot import ActiveMemberReboot


def test_activity_member_reboot():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    tentacle = secondary.tentacle(0)
    tv = TvComponent(tentacle.front, "Reboot TV")
    avr = AvrComponent(tentacle.back, "Reboot AVR")
    player = PlaybackComponent(avr, "Reboot Player", port=1)
    components = (tv, avr, player)

    with TestBed(main, secondary, *components, members=(secondary,), preserve_dut_on_failure=True):
        member = main.add_amity(secondary)
        member.assert_connected()
        scan = main.scan_hdmi()
        scan.assert_contains(*components)
        scan.assert_contains_only(*components)
        for component in components:
            scan.assert_on(component, component.endpoint)

        activity = main.create_activity("Playback through member reboot", display=tv, audio=avr, source=player)
        started = activity.start()
        for component in components:
            component.wait_for_power(PowerState.ON, since=started)
        avr.wait_for_input(player, since=started)
        tv.wait_for_amity_input(since=started)
        activity.assert_active()
        main.press_key(Key.VOLUME_UP).assert_received_by(avr)
        main.press_key(Key.RIGHT).assert_received_by(player)

        with ActiveMemberReboot(activity, member) as reboot:
            reboot.reboot()
            main.press_key(Key.VOLUME_UP).assert_received_by(avr)
            main.press_key(Key.RIGHT).assert_received_by(player)
            reboot.assert_continued()

        stopped = activity.standby()
        for component in components:
            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
