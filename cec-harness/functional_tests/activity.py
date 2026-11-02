"""Activity power, switch input and remote key routing on local/remote owners."""

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent,
                           Key, PowerState)
from functional_tests.hdmi_scan import scanned_devices


def test_activity():
    with scanned_devices() as (dut, tv, avr, player):
        components = (tv, avr, player)
        with dut.create_activity("Functional activity", display=tv, source=player, audio=avr) as activity:
            for component in components:
                component.assert_power(PowerState.STANDBY)

            started = activity.start()
            for component in components:
                component.wait_for_power(PowerState.ON, since=started)

            stopped = activity.standby()
            for component in components:
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")


def test_activity_switch_input():
    with scanned_devices() as (dut, tv, avr, player):
        # No CEC source: select the AVR's requested input for a non-CEC source.
        with dut.create_activity("Functional switch input", display=tv, source=None, audio=avr,
                                 switch_device=avr, switch_input=4) as activity:
            started = activity.start()
            for component in (tv, avr):
                component.wait_for_power(PowerState.ON, since=started)
            avr.wait_for_switch_input(4, since=started)
            player.assert_power(PowerState.STANDBY)
            activity.assert_active()


def _exercise_keys(dut, tv, avr, player):
    with dut.create_activity("Functional remote keys", display=tv, source=player, audio=avr) as activity:
        for component in (tv, avr, player):
            component.assert_power(PowerState.STANDBY)
        started = activity.start()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.ON, since=started)
        avr.wait_for_input(player, since=started)
        activity.assert_active()

        for target, keys in ((avr, (Key.VOLUME_UP, Key.VOLUME_DOWN, Key.MUTE)),
                             (player, (Key.UP, Key.DOWN, Key.LEFT, Key.RIGHT, Key.SELECT,
                                       Key.PLAY, Key.PAUSE, Key.STOP))):
            for key in keys:
                dut.press_key(key, duration_ms=100 if key == Key.VOLUME_UP else 0).assert_received_by(target)
        # The paired remote's combined button alternates Play/Pause after Stop.
        for emitted in (Key.PLAY, Key.PAUSE):
            dut.press_key(Key.PLAY_PAUSE).assert_received_by(player, emitted_key=emitted)
        activity.assert_active()

        stopped = dut.press_key(Key.POWER)
        stopped.wait_for_standby()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")


def test_activity_keys():
    with scanned_devices() as (dut, tv, avr, player):
        _exercise_keys(dut, tv, avr, player)


def test_activity_keys_secondary():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    tentacle = secondary.tentacle(0)
    tv = TvComponent(tentacle.front, "Key TV")
    avr = AvrComponent(tentacle.back, "Key AVR")
    player = PlaybackComponent(avr, "Key Player", port=1)
    components = (tv, avr, player)
    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains(*components)
            scan.assert_contains_only(*components)
            scan.assert_on(tv, tentacle.front)
            scan.assert_on(avr, tentacle.back)
            scan.assert_on(player, tentacle.back)
            _exercise_keys(main, tv, avr, player)
            member.assert_connected()
