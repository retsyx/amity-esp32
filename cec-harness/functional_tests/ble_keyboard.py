"""Real BLE HID input routes overlapping buttons locally and to a member."""

from contextlib import nullcontext

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState
from amity_harness.ble_input import BLEInputEvidence


def _ble_keyboard(*, remote):
    main = DUT("amity-dut")
    target = DUT("amity-dut-2") if remote else main
    connection = target.tentacle(0)
    tv = TvComponent(connection.front, "BLE TV")
    avr = AvrComponent(connection.back, "BLE AVR")
    player = PlaybackComponent(avr, "BLE Player", port=1)
    components = (tv, avr, player)
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
                    started = activity.start()
                    for component in components:
                        component.wait_for_power(PowerState.ON, since=started)
                    avr.wait_for_input(player, since=started)
                    activity.assert_active()
                    with BLEInputEvidence(main, scan, *components) as evidence:
                        evidence.assert_tap(keyboard, player)
                        evidence.assert_overlapping_buttons(keyboard, volume=avr, navigation=player)
                        evidence.assert_disconnect_cleanup(keyboard, avr)
                    activity.assert_active()
                    stopped = activity.standby()
                    for component in components:
                        component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                if remote:
                    owner.assert_connected()


def test_ble_keyboard_local():
    _ble_keyboard(remote=False)


def test_ble_keyboard_remote():
    _ble_keyboard(remote=True)
