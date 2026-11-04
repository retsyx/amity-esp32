"""Real BLE HID input routes overlapping buttons locally and to a member."""

from contextlib import nullcontext

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState
from amity_harness.ble_input import BLEInputEvidence
from amity_harness.ble_activity_input import BLEActivityEvidence


def _ble_keyboard(*, remote):
    main = DUT("amity-dut")
    target = DUT("amity-dut-2") if remote else main
    connection = target.tentacle(0)
    tv = TvComponent(connection.front, "BLE TV")
    avr = AvrComponent(connection.back, "BLE AVR")
    player = PlaybackComponent(avr, "BLE Player", port=1)
    next_player = PlaybackComponent(avr, "BLE Next", port=2, address_type="tuner")
    components = (tv, avr, player, next_player)
    duts = (main, target) if remote else (main,)
    with TestBed(*duts, *components, members=(target,) if remote else ()):
        # Pair before creating any other resources. An existing remote bond
        # belongs to the user and is a failed precondition, never fixture cleanup.
        with main.pair_ble_keyboard() as keyboard:
            with (main.add_amity(target) if remote else nullcontext(main)) as owner:
                if remote:
                    owner.assert_connected()
                scan = main.scan_hdmi()
                scan.assert_contains_only(*components)
                for component in components:
                    scan.assert_on(component, component.endpoint)
                with main.create_activity("Functional BLE keyboard", display=tv, source=player, audio=avr) as activity:
                    with main.create_activity("Functional BLE next", display=None, source=next_player, audio=None) as next_activity:
                        started = activity.start()
                        for component in (tv, avr, player):
                            component.wait_for_power(PowerState.ON, since=started)
                        next_player.assert_power(PowerState.STANDBY)
                        avr.wait_for_input(player, since=started)
                        activity.assert_active()
                        with BLEInputEvidence(main, scan, *components) as evidence:
                            evidence.assert_tap(keyboard, player)
                            evidence.assert_overlapping_buttons(keyboard, volume=avr, navigation=player)
                            evidence.assert_shared_recipient(keyboard, player)
                            evidence.assert_failed_press(keyboard, player)
                            evidence.assert_failed_press(keyboard, player, after_repetition=True)
                        with BLEActivityEvidence(main, scan, *components) as evidence:
                            evidence.assert_transition(keyboard, old_source=player, source=next_player,
                                                       display=None, old_display=tv, routing=avr, volume=avr,
                                                       activity=next_activity, all_up=False)
                            for component in (tv, avr, player):
                                component.assert_power(PowerState.STANDBY)
                            evidence.assert_transition(keyboard, old_source=next_player, source=next_player,
                                                       display=None, old_display=None, routing=avr, volume=None,
                                                       activity=next_activity, all_up=True)
                        next_activity.assert_active()
                        stopped = next_activity.standby()
                        next_player.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        tv.assert_power(PowerState.STANDBY)
                        avr.assert_power(PowerState.STANDBY)
                        player.assert_power(PowerState.STANDBY)
                        restarted = activity.start()
                        for component in (tv, avr, player):
                            component.wait_for_power(PowerState.ON, since=restarted)
                        next_player.assert_power(PowerState.STANDBY)
                        avr.wait_for_input(player, since=restarted)
                        activity.assert_active()
                        with BLEInputEvidence(main, scan, *components) as evidence:
                            evidence.assert_disconnect_cleanup(keyboard, avr)
                        activity.assert_active()
                        stopped = activity.standby()
                        for component in (tv, avr, player):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        next_player.assert_power(PowerState.STANDBY)
                if remote:
                    owner.assert_connected()


def test_ble_keyboard_local():
    _ble_keyboard(remote=False)


def test_ble_keyboard_remote():
    _ble_keyboard(remote=True)
