"""Activity power, switch input and remote key routing on local/remote owners."""

from contextlib import ExitStack, contextmanager, nullcontext

from amity_harness import (DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent,
                           Key, PowerState)
from functional_tests.hdmi_scan import scanned_devices
from functional_tests.qualified_names import same_name_equipment, exercise_qualified_activities
from amity_harness.recovery import LookupScans, assert_returned
from amity_harness.activity_edits import ActiveActivityEdits, ActivityEditObservation
from amity_harness.transitions import ActivityDisplayStandby, observe_no_standby
from amity_harness.qualified_names import observe_no_control


def test_activity():
    with scanned_devices() as (dut, tv, avr, player):
        components = (tv, avr, player)
        with dut.create_activity("Functional activity", display=tv, source=player, audio=avr) as activity:
            for component in components:
                component.assert_power(PowerState.STANDBY)

            started = activity.start()
            for component in components:
                component.wait_for_power(PowerState.ON, since=started)
            tv.wait_for_amity_input(since=started)
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
            tv.active_input = 0x2000 if tv.tv_input == 1 else 0x1000
            with observe_no_standby(*components) as preserved:
                repaired = activity.start()
                for component in components:
                    component.wait_for_power(PowerState.ON, since=repaired)
                tv.wait_for_amity_input(since=repaired)
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


def test_activity_no_display():
    with scanned_devices() as (dut, tv, avr, player):
        with dut.create_activity("Functional TV playback", display=tv, source=player, audio=avr) as watch:
            with dut.create_activity("Functional audio playback", display=None, source=player, audio=avr) as listen:
                started = watch.start()
                for component in (tv, avr, player):
                    component.wait_for_power(PowerState.ON, since=started)
                tv.wait_for_amity_input(since=started)
                avr.wait_for_input(player, since=started)
                watch.assert_active()

                # Observe before the direct handoff: a later Standby state
                # must not hide a transient TV wake, route or key command.
                with ActivityDisplayStandby(tv) as handoff, observe_no_standby(avr, player) as shared:
                    started = listen.start()
                    tv.wait_for_power(PowerState.STANDBY, since=started, command="standby", directed=True)
                    avr.wait_for_input(player, since=started)
                    for component in (avr, player):
                        component.assert_power(PowerState.ON)
                    listen.assert_active()
                    shared.assert_preserved(since=started)

                    # Overlap the observations so there is no gap between
                    # allowing the handoff's Standby and requiring no TV use.
                    with observe_no_control(dut.last_scan, tv) as unused:
                        handoff.assert_standby_only(since=started)
                        dut.press_key(Key.SELECT).assert_received_by(player)
                        dut.press_key(Key.VOLUME_UP).assert_received_by(avr)
                        with observe_no_standby(avr, player) as preserved:
                            announcement = player.activate()
                            assert announcement.ok
                            preserved.assert_preserved_after(announcement, within=0.250)
                        listen.assert_active()

                        player.standby()
                        avr.active_input = 0x1200
                        with observe_no_standby(avr, player) as preserved:
                            repaired = listen.start()
                            for component in (avr, player):
                                component.wait_for_power(PowerState.ON, since=repaired)
                            avr.wait_for_input(player, since=repaired)
                            listen.assert_active()
                            preserved.assert_preserved(since=repaired)
                        dut.press_key(Key.SELECT).assert_received_by(player)
                        dut.press_key(Key.VOLUME_UP).assert_received_by(avr)
                        listen.assert_active()
                        unused.assert_clear()
                    tv.assert_power(PowerState.STANDBY)

                stopped = listen.standby()
                for component in (avr, player):
                    component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                tv.assert_power(PowerState.STANDBY)


@contextmanager
def _clear_backside_on_exit(dut, tv, back):
    """Disconnect the fixture before restoring the adapter's previous default."""
    primary = None
    try:
        yield
    except BaseException as error:
        primary = error
        raise
    finally:
        try:
            for component in back:
                component.power_off()
            cleared = dut.scan_hdmi()
            cleared.assert_contains_only(tv)
            cleared.assert_on(tv, tv.endpoint)
        except Exception as cleanup_error:
            if primary is None:
                raise
            note = getattr(primary, "add_note", None)
            if callable(note):
                note("Back-side fixture cleanup failed: " + type(cleanup_error).__name__)


def _exercise_nondefault_activity(*, receiver, remote=False):
    dut = DUT("amity-dut")
    owner = DUT("amity-dut-2") if remote else dut
    connector = owner.tentacle(0, tv_input=3)
    tv = TvComponent(connector.front, "Input TV")
    avr = AvrComponent(connector.back, "Input AVR") if receiver else None
    player = (PlaybackComponent(avr, "Input Player", port=2) if receiver
              else PlaybackComponent(connector.back, "Input Player"))
    components = (tv, avr, player) if receiver else (tv, player)
    duts = (dut, owner) if remote else (dut,)
    with TestBed(*duts, *components, members=(owner,) if remote else ()):
        assert tv.address.physical == 0
        assert player.address.physical == (0x3200 if receiver else 0x3000)
        if receiver:
            assert avr.address.physical == 0x3000
        with (dut.add_amity(owner) if remote else nullcontext(dut)) as control:
            if remote:
                control.assert_connected()
            scan = dut.scan_hdmi()
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)

            # The explicit UI save selects the scanned TV input on the running
            # adapter; enumeration alone does not change its selected identity.
            with control.front_physical_address(0, "3.0.0.0") as address, _clear_backside_on_exit(dut, tv, components[1:]):
                address.assert_address(detected="3.0.0.0", selected="3.0.0.0")
                with dut.create_activity("Functional nondefault inputs" if receiver else "Functional TV audio",
                                         display=tv, source=player, audio=avr) as activity:
                    saved = next(row for row in dut.get_configuration()["activities"]
                                 if row["activity_id"] == activity.activity_id)
                    if not receiver:
                        assert saved["audio"] == ""

                    for repair in (False, True):
                        if repair:
                            player.standby()
                            tv.active_input = 0x1000
                            if receiver:
                                avr.active_input = 0x3100
                        started = activity.start()
                        for component in components:
                            component.wait_for_power(PowerState.ON, since=started)
                        tv.wait_for_amity_input(since=started)
                        if receiver:
                            avr.wait_for_input(player, since=started)
                        assert player.active
                        activity.assert_active()
                        for key in (Key.RIGHT, Key.SELECT):
                            dut.press_key(key).assert_received_by(player)

                        target = avr if receiver else tv
                        tv_audio = (tv.volume, tv.muted)
                        for key in (Key.VOLUME_UP, Key.VOLUME_DOWN):
                            volume = target.volume
                            dut.press_key(key).assert_received_by(target)
                            if key == Key.VOLUME_UP:
                                assert target.volume > volume
                            else:
                                assert target.volume < volume
                        muted = target.muted
                        dut.press_key(Key.MUTE).assert_received_by(target)
                        assert target.muted != muted
                        if receiver:
                            assert (tv.volume, tv.muted) == tv_audio
                        activity.assert_active()

                    stopped = activity.standby()
                    for component in components:
                        component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
            if remote:
                control.assert_connected()


def test_activity_without_receiver():
    _exercise_nondefault_activity(receiver=False)


def test_activity_without_receiver_secondary():
    _exercise_nondefault_activity(receiver=False, remote=True)


def test_activity_nondefault_inputs():
    _exercise_nondefault_activity(receiver=True)


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
    dut, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    systems = []
    for owner, label in ((dut, "Local"), (secondary, "Remote")):
        connector = owner.tentacle(0)
        tv = TvComponent(connector.front, f"{label} TV")
        avr = AvrComponent(connector.back, f"{label} AVR")
        systems.append((tv, avr, PlaybackComponent(avr, f"{label} Player", port=1)))
    local, remote = systems
    components = (*local, *remote)
    with TestBed(dut, secondary, *components, members=(secondary,)):
        with dut.add_amity(secondary) as member:
            member.assert_connected()
            scan = dut.scan_hdmi()
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)

            # Save the local numeric-input row first. Its edit moves the
            # running member activity's UI index without selecting it again.
            with dut.create_activity("Functional switch input", display=local[0], source=None,
                                     audio=local[1], switch_device=local[1], switch_input=4) as other:
                with dut.create_activity("Functional editing playback", display=remote[0],
                                         source=remote[2], audio=remote[1]) as activity:
                    started = activity.start()
                    for component in remote:
                        component.wait_for_power(PowerState.ON, since=started)
                    for component in local:
                        component.assert_power(PowerState.STANDBY)
                    remote[0].wait_for_amity_input(since=started)
                    remote[1].wait_for_input(remote[2], since=started)
                    activity.assert_active()
                    dut.press_key(Key.RIGHT).assert_received_by(remote[2])
                    dut.press_key(Key.VOLUME_UP).assert_received_by(remote[1])

                    with LookupScans(scan, local[0], local[1], remote[0], remote[1]) as discovery:
                        with ActivityEditObservation(*components, keys=(Key.SELECT, Key.VOLUME_DOWN)) as preserved:
                            edits = ActiveActivityEdits(activity)
                            edits.assert_active_rejected()
                            edits.assert_active_rejected(delete=True)
                            edits.edit_other(other, switch_input=3)
                            # Exercise the member immediately after saving;
                            # receive-coverage observation completes afterward.
                            dut.press_key(Key.SELECT).assert_received_by(remote[2])
                            dut.press_key(Key.VOLUME_DOWN).assert_received_by(remote[1])
                            preserved.assert_preserved()
                        for component in remote:
                            component.assert_power(PowerState.ON)
                        for component in local:
                            component.assert_power(PowerState.STANDBY)
                        activity.assert_active()
                        assert remote[1].active_input == remote[2].address.physical
                        discovery.assert_no_scan()

                    stopped = activity.standby()
                    for component in remote:
                        component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")

                    # Start the same edited row on the main and require its
                    # new numeric input, without any further configuration save.
                    started = other.start()
                    for component in local[:2]:
                        component.wait_for_power(PowerState.ON, since=started)
                    local[0].wait_for_amity_input(since=started)
                    local[1].wait_for_switch_input(3, since=started)
                    local[2].assert_power(PowerState.STANDBY)
                    for component in remote:
                        component.assert_power(PowerState.STANDBY)
                    other.assert_active()
                    dut.press_key(Key.VOLUME_UP).assert_received_by(local[1])
                    stopped = other.standby()
                    for component in local[:2]:
                        component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
            member.assert_connected()


def _exercise_manual_component(owner_name):
    main = DUT("amity-dut")
    owner = main if owner_name == main.name else DUT(owner_name)
    tentacle = owner.tentacle(0)
    tv = TvComponent(tentacle.front, "Manual TV")
    avr = AvrComponent(tentacle.back, "Manual AVR")
    player = PlaybackComponent(avr, "Manual Player", port=1, power=PowerState.OFF)
    members = () if owner is main else (owner,)
    duts = (main,) if owner is main else (main, owner)

    with TestBed(*duts, tv, avr, player, members=members), ExitStack() as stack:
        member = stack.enter_context(main.add_amity(owner)) if members else None
        if member is not None:
            member.assert_connected()
        scan = main.scan_hdmi()
        scan.assert_contains_only(tv, avr)
        scan.assert_on(tv, tentacle.front)
        scan.assert_on(avr, tentacle.back)
        player.assert_power(PowerState.OFF)

        with LookupScans(scan, tv, avr) as discovery:
            # Save the source through Add component while it cannot answer CEC.
            # Neither operation may enumerate this adapter's physical wires.
            with main.create_activity("Functional manual component", display=tv,
                                      source=player, audio=avr,
                                      add_components=(player,)) as activity:
                rows = main.get_configuration()["activities"]
                saved = next(row for row in rows if row["activity_id"] == activity.activity_id)
                selector = saved["source"]
                assert selector.startswith("component:")
                assert all(row["name"] != selector for row in main.get_state()["devices"])
                player.assert_power(PowerState.OFF)
                discovery.assert_no_scan_on_witnesses()

                # Restore CEC power in standby. Require its actual OSD exchange
                # before accepting fresh activity commands as physical proof.
                returned = player.checkpoint()
                player.standby()
                discovery.assert_passive_return(player, since=returned)
                discovery.assert_no_scan_on_witnesses()
                assert any(row["name"] == selector for row in main.get_state()["devices"])
                current = next(row for row in main.get_configuration()["activities"]
                               if row["activity_id"] == activity.activity_id)
                assert current["source"] == selector

                started = activity.start()
                for component in (tv, avr, player):
                    component.wait_for_power(PowerState.ON, since=started)
                tv.wait_for_amity_input(since=started)
                avr.wait_for_input(player, since=started)
                activity.assert_active()
                main.press_key(Key.SELECT).assert_received_by(player)
                main.press_key(Key.VOLUME_UP).assert_received_by(avr)
                discovery.assert_no_scan_on_witnesses()

                stopped = activity.standby()
                for component in (tv, avr, player):
                    component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
        if member is not None:
            member.assert_connected()


def test_activity_manual_component():
    _exercise_manual_component("amity-dut")


def test_activity_manual_component_secondary():
    _exercise_manual_component("amity-dut-2")


def test_activity_same_osd_locations():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-4")
    # The first pair differs only by owner; the second only by adapter.
    systems = (same_name_equipment(main), same_name_equipment(secondary),
               same_name_equipment(secondary, 1))
    components = tuple(component for system in systems for component in system)
    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            exercise_qualified_activities(main, systems,
                                          ("Qualified main", "Qualified member adapter 1",
                                           "Qualified member adapter 2"))
            member.assert_connected()


def _exercise_keys(dut, tv, avr, player):
    with dut.create_activity("Functional remote keys", display=tv, source=player, audio=avr) as activity:
        for component in (tv, avr, player):
            component.assert_power(PowerState.STANDBY)
        started = activity.start()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.ON, since=started)
        tv.wait_for_amity_input(since=started)
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
        tv.wait_for_amity_input(since=started)
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
