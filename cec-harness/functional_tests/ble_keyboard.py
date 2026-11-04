"""Real BLE HID input routes overlapping buttons locally and to a member."""

from contextlib import ExitStack, nullcontext

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, CecAddress
from amity_harness.ble_input import BLEInputEvidence
from amity_harness.ble_activity_input import BLEActivityEvidence
from amity_harness.ble_keyboard import PairedKeyboard


def test_ble_keyboard_lifecycle():
    """Six UI scans retain one advertisement; pair after the fourth and sixth."""
    main = DUT("amity-dut")
    player = PlaybackComponent(main.tentacle(0).back, "BLE Lifecycle", power=PowerState.OFF,
                               address=CecAddress(4, 0x1000))
    with TestBed(main, player) as bed:
        system = main.get_system()
        boot, group = system["boot_id"], system["consortium_id"]
        flags = ("ready", "bonded", "connected", "connecting", "pairing", "unpairing", "scanning")
        observation = {"kind": "ble_keyboard_lifecycle", "device_id": main.identity.device_id,
                       "main_boot": boot, "checks": []}
        if not hasattr(bed, "control_observations"):
            bed.control_observations = []
        bed.control_observations.append(observation)

        def snapshot():
            current = main.get_system()
            if (current.get("octopus_id") != main.identity.device_id or current.get("boot_id") != boot
                    or current.get("consortium_id") != group):
                raise AssertionError("BLE pairing lifecycle changed the main boot or consortium")
            state = main.request("/api/remotes")
            generation = state.get("remote_generation") if isinstance(state, dict) else None
            if (not isinstance(state, dict) or state.get("boot_id") != boot
                    or any(type(state.get(flag)) is not bool for flag in flags)
                    or not isinstance(generation, str) or not generation.isascii() or not generation.isdecimal()):
                raise AssertionError("The remote UI has no valid pairing lifecycle state")
            return {key: state[key] for key in (*flags, "boot_id", "remote_generation")}

        def unpaired(state):
            if not state["ready"] or any(state[flag] for flag in flags if flag != "ready"):
                raise AssertionError("Remote pairing must finish idle and unbonded before the next cycle")

        before = snapshot()
        unpaired(before)
        with PairedKeyboard(main) as keyboard, ExitStack() as activities:
            keyboard.open()
            activity = scan = None
            for number in range(1, 7):
                generation = int(before["remote_generation"])
                check = {"scan": number, "before": before, "passed": False}
                observation["checks"].append(check)
                keyboard.scan()
                scanned = snapshot()
                check["scanned"] = scanned
                unpaired(scanned)
                if scanned["remote_generation"] != str(generation):
                    raise AssertionError("Scanning changed the reviewed remote generation")
                if number in (4, 6):
                    keyboard.pair().assert_paired()
                    paired = snapshot()
                    check["paired"] = paired
                    if (paired["remote_generation"] != str(generation + 1)
                            or not paired["ready"] or not paired["bonded"] or not paired["connected"]):
                        raise AssertionError("Pair did not establish the reviewed next remote generation")
                    if activity is None:
                        # Preserve the fresh commissioning/remote path before
                        # claiming CEC addresses or changing activity settings.
                        player.standby()
                        scan = main.scan_hdmi()
                        scan.assert_contains_only(player)
                        scan.assert_on(player, player.endpoint)
                        activity = activities.enter_context(main.create_activity(
                            "Functional BLE lifecycle", display=None, source=player))
                    rows = main.get_configuration()["activities"]
                    slots = [index for index, row in enumerate(rows)
                             if row.get("activity_id") == activity.activity_id]
                    if len(slots) != 1 or not 0 <= slots[0] <= 3:
                        raise AssertionError("The lifecycle activity needs a saved F1-F4 shortcut")
                    target = slots[0]
                    with keyboard.reports() as reports:
                        reports.report(keyboard=(0x3A + target,))
                        # The selector release admits setup and leaves all keys
                        # up, so this own selection cannot rearm the input gate.
                        activity._needs_standby = True
                        activity._last_start_terminal = False
                        started = reports.all_up()
                    player.wait_for_power(PowerState.ON, since=started)

                    def selected(state):
                        if any(state.get(field) != started.state.get(field) for field in
                               ("expected_boot_id", "expected_config_revision", "expected_presentation_revision")):
                            raise AssertionError("The lifecycle activity changed while BLE setup completed")
                        return state.get("current_activity") == target and state.get("activity_settling") is False

                    main.wait_for(selected, timeout=main._runtime.timeout)
                    activity._last_start_terminal = True
                    activity.assert_active()
                    with BLEInputEvidence(main, scan, player) as evidence:
                        evidence.assert_tap(keyboard, player)
                    stopped = activity.standby()
                    player.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                    keyboard.unpair()
                    after = snapshot()
                    check["after"] = after
                    unpaired(after)
                    if after["remote_generation"] != str(generation + 2):
                        raise AssertionError("Unpair did not finish the reviewed next remote generation")
                    before = after
                else:
                    before = scanned
                check["passed"] = True


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
