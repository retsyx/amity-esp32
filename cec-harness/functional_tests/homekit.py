"""Control the shipping HomeKit television with real HAP and physical CEC."""

from contextlib import nullcontext

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.homekit import HomeKitAccessory
from amity_harness.homekit_input import _input_slots, _select, _key, _rejected, _standby, _boundary, _settled


REMOTE_KEYS = ((0, 0x48), (1, 0x49), (2, 0x4B), (3, 0x4C),
               (4, Key.UP), (5, Key.DOWN), (6, Key.LEFT), (7, Key.RIGHT),
               (8, Key.SELECT), (9, 0x0D), (10, 0x0D), (15, 0x35))


def _assert_state(homekit, *, active, identifier):
    state = homekit.read_state()
    if state != {"active": active, "current_input": identifier, "speaker_active": active}:
        raise AssertionError("HomeKit power and current input do not match the settled activity")


def _started(action, tv, avr, player, other):
    for component in (tv, avr, player):
        component.wait_for_power(PowerState.ON, since=action)
    other.assert_power(PowerState.STANDBY)
    tv.wait_for_amity_input(since=action)
    avr.wait_for_input(player, since=action)


def _stopped(action, tv, avr, player, other):
    for component in (tv, avr, player):
        component.wait_for_power(PowerState.STANDBY, since=action, command="standby")
    other.assert_power(PowerState.STANDBY)


def _homekit_accessory(*, remote):
    main = DUT("amity-dut")
    target = DUT("amity-dut-2") if remote else main
    connection = target.tentacle(0)
    tv = TvComponent(connection.front, "HomeKit TV")
    avr = AvrComponent(connection.back, "HomeKit AVR")
    first = PlaybackComponent(avr, "HomeKit First", port=1)
    second = PlaybackComponent(avr, "HomeKit Second", port=2, address_type="tuner")
    components = (tv, avr, first, second)
    duts = (main, target) if remote else (main,)
    with TestBed(*duts, *components, members=(target,) if remote else ()):
        with (main.add_amity(target) if remote else nullcontext(main)) as owner:
            if remote:
                owner.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)
            with main.create_activity("Functional HomeKit first", display=tv, source=first, audio=avr) as initial:
                with main.create_activity("Functional HomeKit second", display=tv, source=second, audio=avr) as following:
                    with HomeKitAccessory(main) as homekit:
                        slots = _input_slots(homekit, main, (initial, following))
                        _assert_state(homekit, active=False, identifier=0)
                        started = _select(homekit, initial, slots[initial.activity_id])
                        _started(started, tv, avr, first, second)
                        _assert_state(homekit, active=True, identifier=slots[initial.activity_id])
                        for remote_key, cec_key in REMOTE_KEYS:
                            _key(homekit, main, first, cec_key,
                                 lambda remote_key=remote_key: homekit.send_key(remote_key))
                        for cec_key, playing in ((Key.PLAY, True), (Key.PAUSE, False)):
                            _key(homekit, main, first, cec_key, lambda: homekit.send_key(11))
                            if first.playing is not playing:
                                raise AssertionError("HomeKit Play/Pause did not change source playback")
                        for direction, cec_key in ((1, Key.VOLUME_UP), (-1, Key.VOLUME_DOWN)):
                            volume = avr.volume
                            _key(homekit, main, avr, cec_key,
                                 lambda direction=direction: homekit.step_volume(direction))
                            if avr.volume != volume + direction:
                                raise AssertionError("HomeKit volume did not change the selected receiver")
                        for muted, cec_key in ((True, 0x65), (False, 0x66)):
                            _key(homekit, main, avr, cec_key, lambda muted=muted: homekit.set_mute(muted))
                            if avr.muted is not muted:
                                raise AssertionError("HomeKit mute did not change the selected receiver")
                        _rejected(homekit, main, scan, initial, first)
                        _assert_state(homekit, active=True, identifier=slots[initial.activity_id])
                        changed = _select(homekit, following, slots[following.activity_id])
                        first.wait_for_power(PowerState.STANDBY, since=changed, command="standby", directed=True)
                        second.wait_for_power(PowerState.ON, since=changed)
                        for component in (tv, avr):
                            component.assert_power(PowerState.ON)
                        tv.wait_for_amity_input(since=changed)
                        avr.wait_for_input(second, since=changed)
                        _assert_state(homekit, active=True, identifier=slots[following.activity_id])
                        _key(homekit, main, second, Key.RIGHT, lambda: homekit.send_key(7))
                        stopped = _standby(homekit, main, scan)
                        _stopped(stopped, tv, avr, second, first)
                        _assert_state(homekit, active=False, identifier=0)
                        # Television Active starts the first saved activity from standby.
                        restarted = _boundary(main, scan, "homekit_power_on")
                        initial._needs_standby = True
                        initial._last_start_terminal = False
                        if homekit.set_power(True) != 0:
                            raise AssertionError("HomeKit rejected television power on")
                        _settled(restarted, initial)
                        _started(restarted, tv, avr, first, second)
                        _assert_state(homekit, active=True, identifier=slots[initial.activity_id])
                        _key(homekit, main, first, Key.SELECT, lambda: homekit.send_key(8))
                        stopped = _standby(homekit, main, scan)
                        _stopped(stopped, tv, avr, first, second)
                        _assert_state(homekit, active=False, identifier=0)
            if remote:
                owner.assert_connected()


def test_homekit_accessory_local():
    _homekit_accessory(remote=False)


def test_homekit_accessory_remote():
    _homekit_accessory(remote=True)
