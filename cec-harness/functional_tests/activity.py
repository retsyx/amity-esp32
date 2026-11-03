"""Activity power, switch input and remote key routing on local/remote owners."""

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent,
                           Key, PowerState)
from functional_tests.hdmi_scan import scanned_devices
from amity_harness.recovery import LookupScans, assert_returned
from amity_harness.transitions import observe_no_standby


def test_activity():
    with scanned_devices() as (dut, tv, avr, player):
        components = (tv, avr, player)
        with dut.create_activity("Functional activity", display=tv, source=player, audio=avr) as activity:
            for component in components:
                component.assert_power(PowerState.STANDBY)

            started = activity.start()
            for component in components:
                component.wait_for_power(PowerState.ON, since=started)
            avr.wait_for_input(player, since=started)
            activity.assert_active()

            with observe_no_standby(*components) as preserved:
                announcement = player.activate()
                assert announcement.ok
                preserved.assert_preserved_after(announcement, within=0.250)
            for component in components:
                component.assert_power(PowerState.ON)
            assert player.active
            activity.assert_active()

            # Disturb the fixture, then reselect this exact saved activity.
            # Only fresh DUT commands can prove that the repair worked.
            player.standby()
            avr.active_input = 0x1200
            with observe_no_standby(*components) as preserved:
                repaired = activity.start()
                for component in components:
                    component.wait_for_power(PowerState.ON, since=repaired)
                avr.wait_for_input(player, since=repaired)
                activity.assert_active()
                preserved.assert_preserved(since=repaired)

            stopped = activity.standby()
            for component in components:
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")

            # The former source is now a thief: global standby broadcasts
            # Standby rather than selecting an activity or a directed response.
            announcement = player.activate()
            assert announcement.ok
            player.wait_for_power(PowerState.STANDBY, since=announcement.timestamp_ns,
                                  command="standby", broadcast=True)
            for component in components:
                component.assert_power(PowerState.STANDBY)
            state = dut.get_state()
            assert state["current_activity"] == -1
            assert state["activity_settling"] is False


def test_activity_unselected_equipment_announcement():
    dut = DUT("amity-dut")
    tentacle = dut.tentacle(0)
    tv = TvComponent(tentacle.front, "Idle TV")
    avr = AvrComponent(tentacle.back, "Idle AVR")
    player = PlaybackComponent(avr, "Idle Player", port=1, power=PowerState.OFF)

    with TestBed(dut, tv, avr, player):
        scan = dut.scan_hdmi()
        scan.assert_contains(tv, avr)
        scan.assert_contains_only(tv, avr)
        scan.assert_on(tv, tentacle.front)
        scan.assert_on(avr, tentacle.back)
        with dut.create_activity("Functional standby cache", display=tv, source=None, audio=avr):
            # Never select this source. Its first announcement must populate
            # the UI cache and request its identity without enumerating peers.
            with LookupScans(scan, tv, avr) as announcements:
                announced = player.checkpoint()
                player.standby()
                assert_returned(scan, player, since=announced)
                announcements.assert_no_scan()
            for component in (tv, avr, player):
                component.assert_power(PowerState.STANDBY)
            state = dut.get_state()
            assert state["current_activity"] == -1
            assert state["activity_settling"] is False


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


def _exercise_disconnected_key(dut, tv, avr, player):
    with dut.create_activity("Functional disconnected hold", display=tv, source=player, audio=avr) as activity:
        started = activity.start()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.ON, since=started)
        avr.wait_for_input(player, since=started)
        activity.assert_active()

        dut.press_key_disconnected(Key.VOLUME_UP, avr)
        dut.press_key(Key.SELECT).assert_received_by(player)
        activity.assert_active()

        stopped = activity.standby()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")


def test_activity_keys_disconnected():
    with scanned_devices() as (dut, tv, avr, player):
        _exercise_disconnected_key(dut, tv, avr, player)


def _secondary_keys(exercise):
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
            exercise(main, tv, avr, player)
            member.assert_connected()


def test_activity_keys_secondary():
    _secondary_keys(_exercise_keys)


def test_activity_keys_disconnected_secondary():
    _secondary_keys(_exercise_disconnected_key)
