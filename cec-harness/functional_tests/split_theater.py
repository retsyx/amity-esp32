"""Independent activity progress across a source-side and a display-side Amity."""

from contextlib import contextmanager

from amity_harness import (AvrAttachment, AvrComponent, CecAddress, DUT, Key,
                           PlaybackComponent, PowerState, TestBed, TvComponent)
from amity_harness.recovery import LookupScans
from amity_harness.split_theater import LookupNameGate


@contextmanager
def _clear_source_on_exit(main, player, known):
    """Clear the nondefault source connection before restoring adapter settings."""
    primary = None
    try:
        yield
    except BaseException as error:
        primary = error
        raise
    finally:
        try:
            player.power_off()
            cleared = main.scan_hdmi()
            cleared.assert_contains_only(*known)
            for component in known:
                cleared.assert_on(component, component.endpoint)
        except Exception as cleanup_error:
            if primary is None:
                raise
            note = getattr(primary, "add_note", None)
            if callable(note):
                note("Split source cleanup failed: " + type(cleanup_error).__name__)


def _exercise_split_theater(*, source_on_main):
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    source_owner, display_owner = (main, secondary) if source_on_main else (secondary, main)
    source_connection, output = source_owner.tentacle(0), display_owner.tentacle(0)
    tv = TvComponent(output.front, "Split TV")
    avr = AvrComponent(output.back, "Split AVR")
    avr_input = AvrAttachment(avr, source_connection.front)
    player = PlaybackComponent(source_connection.back, "Split Player", address=CecAddress(4, "1.2.0.0"))
    components, known = (tv, avr, avr_input, player), (tv, avr, avr_input)

    with TestBed(main, secondary, *components, members=(secondary,)):
        assert tv.address.physical == 0
        assert avr.address == avr_input.address == CecAddress(5, "1.0.0.0")
        assert player.address.physical == 0x1200
        with main.add_amity(secondary) as member:
            member.assert_connected()
            initial = main.scan_hdmi()
            initial.assert_contains_only(*components)
            for component in components:
                initial.assert_on(component, component.endpoint)

            source_control = main if source_on_main else member
            with source_control.front_physical_address(0, "1.2.0.0") as address, _clear_source_on_exit(main, player, known):
                address.assert_address(detected="1.2.0.0", selected="1.2.0.0")
                with main.create_activity("Functional split theater", display=tv, audio=avr, source=player) as activity:
                    # Suppress only the fixture source's name response before
                    # this explicit scan removes its previously known identity.
                    with LookupNameGate(player, initial, progress=(tv, avr)) as gate:
                        missing = main.scan_hdmi()
                        missing.assert_contains(*known)
                        for component in known:
                            missing.assert_on(component, component.endpoint)
                        gate.set_missing_scan(missing)

                        # Explicit scans refresh the real miss cooldown. Wait
                        # for it so this start's actual missing recipient can
                        # trigger its permitted owner-local lookup scan.
                        with LookupScans(missing, *known) as cooldown:
                            cooldown.wait_for_cooldown()
                            with activity.start_pending(scan=missing) as pending:
                                try:
                                    action = pending.action()
                                    gate.wait_for_progress(since=action)
                                finally:
                                    # Open the gate before pending-start cleanup
                                    # can wait for the original operation.
                                    gate.release()
                                started = pending.wait()
                            # Only the source owner's back side may scan,
                            # once, for this cache miss.
                            cooldown.assert_lookup_of(player)

                        # Validate applied effects after releasing the lookup.
                        # B's commands may precede A's query; their only shared
                        # boundary is the original accepted activity selection.
                        tv.wait_for_power(PowerState.ON, since=action)
                        tv.wait_for_amity_input(since=action)
                        avr.wait_for_power(PowerState.ON, since=action)
                        # The main can finish before the source owner's lookup.
                        # Finish that same scan's identity proof before applying
                        # the ordinary CEC command deadline to the resumed work.
                        missing.assert_contains(*components)
                        player.wait_for_power(PowerState.ON, since=gate.released_at)
                        player.wait_for_source_request(since=gate.released_at)
                        avr_input.wait_for_active_source(player, since=started)
                        for component in (tv, avr, player):
                            component.assert_power(PowerState.ON)
                        activity.assert_active()
                        # Use the returned source's fresh scan for later actions.
                        activity.refresh_scan(missing)
                        main.press_key(Key.SELECT).assert_received_by(player)
                        main.press_key(Key.VOLUME_UP).assert_received_by(avr)
                        activity.assert_active()

                        stopped = activity.standby()
                        for component in (tv, avr, player):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        avr_input.assert_power(PowerState.STANDBY)
            member.assert_connected()


def test_split_theater_main_source():
    _exercise_split_theater(source_on_main=True)


def test_split_theater_member_source():
    _exercise_split_theater(source_on_main=False)
