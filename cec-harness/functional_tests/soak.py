"""Explicit two-hour, four-owner black-box endurance test on the harness Pi."""

from contextlib import nullcontext
import time

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent,
                           PlaybackComponent, PowerState, Key)
from amity_harness.ble_input import BLEInputEvidence
from amity_harness.matter import MatterAccessory
from amity_harness.matter_input import (_input_slots, _select as matter_select, _key as matter_key,
                                       _rejected as matter_rejected, _standby as matter_standby)
from amity_harness.soak_browser import SoakBrowser
from amity_harness.trace import event
from amity_harness.transitions import observe_no_standby


DURATION_SECONDS = 2 * 60 * 60
ACTIVITY_SECONDS = 20
KEY_INTERVAL_SECONDS = 5


def _system(dut, number):
    connection = dut.tentacle(0)
    tv = TvComponent(connection.front, f"Soak{number} TV")
    avr = AvrComponent(connection.back, f"Soak{number} AVR")
    players = tuple(PlaybackComponent(avr, f"Soak{number} Player{port}", port=port,
                                      address_type="playback" if port == 1 else "tuner")
                    for port in (1, 2))
    return tv, avr, players


def _wait_until(deadline, session):
    while True:
        session.assert_healthy()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(1, remaining))


def _select(activity, selected, previous, components, browser, matter, slots, *, route):
    shared = tuple(component for component in selected if component in previous)
    with (observe_no_standby(*shared) if shared else nullcontext()) as preserved:
        if route == "matter":
            started = matter_select(matter, activity, slots[activity.activity_id])
        else:
            started = browser.select(activity) if route == "ui" else activity.start()
        for component in previous:
            if component not in selected:
                component.wait_for_power(PowerState.STANDBY, since=started, command="standby")
        for component in selected:
            if component in shared:
                component.assert_power(PowerState.ON)
            else:
                component.wait_for_power(PowerState.ON, since=started)
        tv, avr, player = selected
        tv.wait_for_amity_input(since=started)
        avr.wait_for_input(player, since=started)
        activity.assert_active()
        if shared:
            preserved.assert_preserved(since=started)
    for component in components:
        component.assert_power(PowerState.ON if component in selected else PowerState.STANDBY)
    browser.assert_active(activity)


def _released(components):
    for component in components:
        if component.pressed_key is not None:
            raise AssertionError("A component retained a pressed key after its final release")


def _keys(main, activity, selected, components, browser, keyboard, scan, matter, proof, activity_index):
    # Each accepted selection arms the input gate. Deliver a real all-up HID
    # state after setup, before any transport starts this activity's keys.
    with keyboard.reports() as reports:
        reports.all_up()
    _, avr, player = selected
    with BLEInputEvidence(main, scan, *components) as evidence:
        deadline = time.monotonic() + ACTIVITY_SECONDS
        for index, (key, target, duration, usage, consumer) in enumerate((
                (Key.RIGHT, player, 0, 0x4F, None), (Key.VOLUME_UP, avr, 750, None, 0xE9),
                (Key.LEFT, player, 650, 0x50, None), (Key.VOLUME_DOWN, avr, 0, None, 0xEA))):
            route = ("ble", "api", "matter")[(proof["sweeps"] + activity_index + index) % 3]
            if route == "ble":
                evidence.assert_key(keyboard, target, key=key, keyboard_usage=usage,
                                    consumer_usage=consumer, duration_ms=duration)
                proof["ble_key_presses"] += 1
            elif route == "api":
                main.press_key(key, duration_ms=duration).assert_received_by(target)
                proof["api_key_presses"] += 1
            else:
                if key in (Key.VOLUME_UP, Key.VOLUME_DOWN):
                    command = lambda: matter.step_volume(1 if key == Key.VOLUME_UP else -1)
                else:
                    command = lambda: matter.send_key(key)
                matter_key(matter, main, target, key, command)
                proof["matter_key_presses"] += 1
            proof["key_presses"] += 1
            _released(components)
            _wait_until(deadline - ACTIVITY_SECONDS + (index + 1) * KEY_INTERVAL_SECONDS,
                        main._testbed.session)
    # Repeat playback and explicit mute controls across every activity/owner.
    extra = (proof["sweeps"] + activity_index) % 7
    if extra < 5:
        name, key = (("play", Key.PLAY), ("pause", Key.PAUSE), ("stop", Key.STOP),
                     ("previous", 0x4C), ("next", 0x4B))[extra]
        matter_key(matter, main, player, key, lambda: matter.playback(name))
    else:
        muted = extra == 5
        matter_key(matter, main, avr, 0x65 if muted else 0x66, lambda: matter.set_mute(muted))
    proof["key_presses"] += 1
    proof["matter_key_presses"] += 1
    _released(components)
    # Rotate rejection/recovery checks through all eight activities, keeping
    # one batch per sweep rather than dominating the workload with rejections.
    if activity_index == proof["sweeps"] % 8:
        matter_rejected(matter, main, scan, activity, player)
        proof["matter_rejection_checks"] += 1
        proof["key_presses"] += 1
        proof["matter_key_presses"] += 1
        _released(components)
    activity.assert_active()
    browser.assert_active(activity)


def _soak(main_name):
    main = DUT(main_name)
    owners = (main, *(DUT(name, wiring=main.wiring) for name in main.wiring.identities if name != main.name))
    if len(owners) != 4:
        raise ValueError("The soak scenario requires four documented Amities")
    systems = tuple(_system(dut, number) for number, dut in enumerate(owners, 1))
    components = tuple(component for tv, avr, players in systems for component in (tv, avr, *players))
    with TestBed(*owners, *components, members=owners[1:], preserve_dut_on_failure=True) as bed:
        proof = {"duration_seconds": DURATION_SECONDS, "elapsed_seconds": 0,
                 "sweeps": 0, "activity_changes": 0, "ui_activity_changes": 0,
                 "api_activity_changes": 0, "matter_activity_changes": 0,
                 "key_presses": 0, "api_key_presses": 0, "ble_key_presses": 0,
                 "matter_key_presses": 0, "matter_rejection_checks": 0, "matter_standbys": 0,
                 "completed": False}
        bed.control_observations = [{"kind": "soak", "checks": [proof]}]
        # The bed owns cleanup, so failures preserve configuration for inspection.
        # Successful cleanup removes activities before members, with CEC still live.
        for member in owners[1:]:
            main.add_amity(member).assert_connected()
        scan = main.scan_hdmi()
        scan.assert_contains_only(*components)
        for component in components:
            scan.assert_on(component, component.endpoint)
            component.assert_power(PowerState.STANDBY)
        proof["owner_boots"] = dict(scan.owner_boots)
        selections = [(main.create_activity(f"Soak {number} source {port}",
                                            display=tv, audio=avr, source=player), (tv, avr, player))
                      for number, (tv, avr, players) in enumerate(systems, 1)
                      for port, player in enumerate(players, 1)]
        # Pair only after direct Add has finished on all members. The owned
        # keyboard context releases input and removes its bond on every exit.
        with main.pair_ble_keyboard() as keyboard, \
                SoakBrowser(main, [activity for activity, _ in selections]) as browser, \
                MatterAccessory(main) as matter:
            proof["ui"] = browser.report
            slots = _input_slots(matter, main, [activity for activity, _ in selections])
            browser.inspect_devices()
            started = time.monotonic()
            previous = ()
            try:
                while time.monotonic() - started < DURATION_SECONDS:
                    for index, (activity, selected) in enumerate(selections):
                        route = ("ui", "api", "matter")[(proof["sweeps"] + index) % 3]
                        _select(activity, selected, previous, components, browser, matter, slots, route=route)
                        proof["activity_changes"] += 1
                        proof[route + "_activity_changes"] += 1
                        _keys(main, activity, selected, components, browser, keyboard, scan, matter, proof, index)
                        previous = selected
                    if proof["sweeps"] % 2:
                        stopped = matter_standby(matter, main, scan)
                        proof["matter_standbys"] += 1
                    else:
                        stopped = browser.standby()
                    for component in previous:
                        component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                    for component in components:
                        component.assert_power(PowerState.STANDBY)
                        if component.pressed_key is not None:
                            raise AssertionError("A key remained pressed in standby")
                    main.wait_for(lambda state: state.get("current_activity") == -1
                                  and state.get("activity_settling") is False)
                    browser.assert_standby()
                    scan.validate_owners(scan.expected)
                    browser.inspect_devices()
                    previous = ()
                    proof["sweeps"] += 1
                    proof["elapsed_seconds"] = round(time.monotonic() - started, 3)
                    event("soak.progress", **proof)
                proof["completed"] = True
            finally:
                proof["elapsed_seconds"] = round(time.monotonic() - started, 3)


def test_soak():
    """First phase of the explicit soak run: two hours with the original main."""
    _soak("amity-dut")


def test_soak_main4():
    """Second phase of the explicit soak run: two hours with the fourth board as main."""
    _soak("amity-dut-4")
