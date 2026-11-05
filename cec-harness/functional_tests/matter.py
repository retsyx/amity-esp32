"""Exercise the shipping Matter accessory against physical CEC recipients."""

from contextlib import nullcontext

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.matter import MatterAccessory
from amity_harness.matter_input import _input_slots, _select, _key, _rejected, _standby


KEYPAD_KEYS = (*range(0x0E), 0x10, 0x11, *range(0x20, 0x2A))


def _matter_accessory(*, remote):
    main = DUT("amity-dut")
    target = DUT("amity-dut-2") if remote else main
    connection = target.tentacle(0)
    tv = TvComponent(connection.front, "Matter TV")
    avr = AvrComponent(connection.back, "Matter AVR")
    first = PlaybackComponent(avr, "Matter First", port=1)
    second = PlaybackComponent(avr, "Matter Second", port=2, address_type="tuner")
    components = (tv, avr, first, second)
    duts = (main, target) if remote else (main,)
    with TestBed(*duts, *components, members=(target,) if remote else ()) as bed:
        with (main.add_amity(target) if remote else nullcontext(main)) as owner:
            if remote:
                owner.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)
            with main.create_activity("Functional Matter first", display=tv, source=first, audio=avr) as initial:
                with main.create_activity("Functional Matter second", display=tv, source=second, audio=avr) as following:
                    with MatterAccessory(main) as matter:
                        slots = _input_slots(matter, main, (initial, following))
                        started = _select(matter, initial, slots[initial.activity_id])
                        for component in (tv, avr, first):
                            component.wait_for_power(PowerState.ON, since=started)
                        second.assert_power(PowerState.STANDBY)
                        tv.wait_for_amity_input(since=started)
                        avr.wait_for_input(first, since=started)
                        for code in KEYPAD_KEYS:
                            _key(matter, main, first, code, lambda code=code: matter.send_key(code))
                        for command, code in (("play", Key.PLAY), ("pause", Key.PAUSE), ("stop", Key.STOP),
                                              ("previous", 0x4C), ("next", 0x4B)):
                            _key(matter, main, first, code, lambda command=command: matter.playback(command))
                        for steps, code in ((1, Key.VOLUME_UP), (-1, Key.VOLUME_DOWN)):
                            _key(matter, main, avr, code, lambda steps=steps: matter.step_volume(steps))
                        for muted, code in ((True, 0x65), (False, 0x66)):
                            _key(matter, main, avr, code, lambda muted=muted: matter.set_mute(muted))
                        _rejected(matter, main, scan, initial, first)
                        changed = _select(matter, following, slots[following.activity_id])
                        first.wait_for_power(PowerState.STANDBY, since=changed, command="standby", directed=True)
                        second.wait_for_power(PowerState.ON, since=changed)
                        for component in (tv, avr):
                            component.assert_power(PowerState.ON)
                        tv.wait_for_amity_input(since=changed)
                        avr.wait_for_input(second, since=changed)
                        _key(matter, main, second, Key.RIGHT, lambda: matter.send_key(Key.RIGHT))
                        stopped = _standby(matter, main, scan)
                        for component in (tv, avr, second):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        first.assert_power(PowerState.STANDBY)
            if remote:
                owner.assert_connected()


def test_matter_accessory_local():
    _matter_accessory(remote=False)


def test_matter_accessory_remote():
    _matter_accessory(remote=True)
