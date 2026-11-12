"""Replace an added Amity while preserving its saved activities and equipment."""

from contextlib import ExitStack

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent,
                           Key, PowerState)
from amity_harness.qualified_names import observe_no_control
from amity_harness.recovery import assert_switch_commands
from amity_harness.replacement import ReplacementReview


def _equipment(dut, adapter, name):
    connector = dut.tentacle(adapter)
    tv = TvComponent(connector.front, f"{name} TV")
    avr = AvrComponent(connector.back, f"{name} AVR")
    player = PlaybackComponent(avr, f"{name} Player", port=1)
    return tv, avr, player


def _exercise(main, activity, equipment, inactive, *, source=True):
    scan = main.last_scan
    activity.refresh_scan(scan)
    tv, avr, player = equipment
    selected = equipment if source else (tv, avr)
    for component in (*equipment, *inactive):
        component.assert_power(PowerState.STANDBY)

    with observe_no_control(scan, *inactive) as observation:
        started = activity.start()
        for component in selected:
            component.wait_for_power(PowerState.ON, since=started)
        if source:
            avr.wait_for_input(player, since=started)
            if activity.switch_input:
                assert_switch_commands(avr, activity.switch_input, since=started, count=0)
            main.press_key(Key.RIGHT).assert_received_by(player)
            main.press_key(Key.SELECT).assert_received_by(player)
        else:
            avr.wait_for_switch_input(activity.switch_input, since=started)
            assert_switch_commands(avr, activity.switch_input, since=started, count=1)
            player.assert_power(PowerState.STANDBY)
        main.press_key(Key.VOLUME_UP).assert_received_by(avr)
        activity.assert_active()
        for component in inactive:
            component.assert_power(PowerState.STANDBY)
        # Standby must not reach the same-named inactive equipment.
        stopped = activity.standby()
        for component in selected:
            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
        observation.assert_clear()

    for component in (*equipment, *inactive):
        component.assert_power(PowerState.STANDBY)


def test_amity_replacement():
    main, previous, replacement = DUT("amity-dut"), DUT("amity-dut-2"), DUT("amity-dut-4")
    local = _equipment(main, 0, "Local")
    old = _equipment(previous, 0, "Same")
    new = _equipment(replacement, 1, "Same")
    components = (*local, *old, *new)

    with TestBed(main, previous, replacement, *components, members=(previous, replacement)):
        with ExitStack() as resources:
            for component in new:
                component.power_off()
            removed = resources.enter_context(main.add_amity(previous))
            removed.assert_connected()
            added = resources.enter_context(main.add_amity(replacement))
            added.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*local, *old)
            for component in (*local, *old):
                scan.assert_on(component, component.endpoint)

            keeper = resources.enter_context(main.create_activity(
                "Replacement local", display=local[0], audio=local[1], source=local[2]))
            playback = resources.enter_context(main.create_activity(
                "Replacement playback", display=old[0], audio=old[1], source=old[2],
                switch_device=old[1], switch_input=4))
            numeric = resources.enter_context(main.create_activity(
                "Replacement numeric input", display=old[0], audio=old[1], source=None,
                switch_device=old[1], switch_input=3))

            _exercise(main, keeper, local, old)
            _exercise(main, playback, old, local)
            _exercise(main, numeric, old, local, source=False)

            # The paired responders represent the equipment moved between the
            # boards. Only its new HDMI connections can answer after this move.
            for component in old:
                component.power_off()
            for component in new:
                component.standby()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*local, *new)
            for component in (*local, *new):
                scan.assert_on(component, component.endpoint)
            review = ReplacementReview(removed, added, (keeper, playback, numeric),
                                       tuple(zip(old, new, strict=True)))
            review.preview()
            review.apply()

            _exercise(main, keeper, local, new)
            _exercise(main, playback, new, local)
            _exercise(main, numeric, new, local, source=False)
            review.assert_retained()
