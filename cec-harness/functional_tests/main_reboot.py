"""Members adopt no activity when only the main reboots during playback."""

from amity_harness import (AvrAttachment, AvrComponent, CecAddress, DUT, PlaybackComponent,
                           PowerState, TestBed, TvComponent)
from amity_harness.qualified_names import observe_no_control
from amity_harness.reboot import MainReboot
from amity_harness.transitions import ActivityHandoff, observe_no_standby

# After the last expected command, the wires must stay unchanged this long.
HOLD_SECONDS = 3


def test_activity_main_reboot():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-4")
    display, game_input, movie_input = main.tentacle(0), secondary.tentacle(0), secondary.tentacle(1)
    tv = TvComponent(display.front, "Restart TV")
    # The AVR's output reaches the main's tentacle without CEC. Its inputs 2
    # and 3 attach to the member's tentacles 0 and 1, so the member alone
    # controls the AVR and both sources, and the main owns only the TV.
    avr = AvrComponent(game_input.front, "Restart AVR", address=CecAddress(5, "1.0.0.0"))
    console = PlaybackComponent(game_input.back, "Game Console", address=CecAddress(4, "1.2.0.0"))
    avr_input = AvrAttachment(avr, movie_input.front)
    player = PlaybackComponent(movie_input.back, "Movie Player", address=CecAddress(4, "1.3.0.0"))
    # Every other wired member adapter holds equipment no activity uses.
    spares = []
    for index in sorted({index for name, index, _ in secondary.wiring.cec if name == secondary.name} - {0, 1}):
        connection = secondary.tentacle(index)
        spares.append((TvComponent(connection.front, f"Spare TV {index}"),
                       PlaybackComponent(connection.back, f"Spare Player {index}")))
    components = (tv, avr, console, avr_input, player, *(component for pair in spares for component in pair))

    with TestBed(main, secondary, *components, members=(secondary,), preserve_dut_on_failure=True):
        member = main.add_amity(secondary)
        member.assert_connected()
        scan = main.scan_hdmi()
        scan.assert_contains(*components)
        scan.assert_contains_only(*components)
        for component in components:
            scan.assert_on(component, component.endpoint)
        # No EDID fixture: the ordinary adapter settings select each AVR input.
        for index, address in ((0, "1.2.0.0"), (1, "1.3.0.0")):
            member.front_physical_address(index, address).assert_address(detected=address, selected=address)

        game = main.create_activity("Game after restart", display=tv, audio=avr, source=console)
        movie = main.create_activity("Movie across restart", display=tv, audio=avr, source=player)
        for component in components:
            component.assert_power(PowerState.STANDBY)
        started = movie.start()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.ON, since=started)
        tv.wait_for_amity_input(since=started)
        player.wait_for_source_request(since=started)
        avr_input.wait_for_active_source(player, since=started)
        movie.assert_active()

        # The member enforces the movie: its selected player may announce itself.
        with observe_no_standby(tv, avr, avr_input, player) as preserved:
            announcement = player.activate()
            assert announcement.ok
            preserved.assert_preserved_after(announcement, within=0.250)

        # Only the main reboots. Every device stays as it was, and the member
        # sends nothing until the next user action: the helper's HDMI scan.
        with MainReboot(movie, (member,), saved=(game,), hold=HOLD_SECONDS) as reboot:
            reboot.reboot()
            # Nothing is current on any member adapter. A source announcing
            # itself, even the movie's own player, gets Standby broadcast on
            # both wires of its adapter, and nothing else follows.
            for front, source in ((avr, console), (avr_input, player), *spares):
                reboot.assert_no_activity_policy(front, source)

        # The new boot's first selection starts from no activity. It powers on
        # everything it uses and sends nothing to the movie's player, which no
        # current activity uses.
        with observe_no_control(main.last_scan, avr_input, player, duration=HOLD_SECONDS) as untouched:
            started = game.start()
            for component in (tv, avr, console):
                component.wait_for_power(PowerState.ON, since=started)
            tv.wait_for_amity_input(since=started)
            console.wait_for_source_request(since=started)
            avr.wait_for_active_source(console, since=started)
            game.assert_active()
            untouched.assert_clear()

        # The next selection has an old activity. On the member, the console's
        # Standby completes before the player's power-on, and that before the
        # player's Set Stream Path. The shared AVR and TV get no power command.
        with (ActivityHandoff(console, player, shared=(avr, avr_input), receiver=player,
                              source=player) as handoff,
              observe_no_standby(tv) as display_kept):
            switched = movie.start()
            console.wait_for_power(PowerState.STANDBY, since=switched, command="standby", directed=True)
            player.wait_for_power(PowerState.ON, since=switched)
            player.wait_for_source_request(since=switched)
            avr_input.wait_for_active_source(player, since=switched)
            tv.wait_for_amity_input(since=switched)
            movie.assert_active()
            handoff.assert_ordered(since=switched)
            display_kept.assert_preserved(since=switched)

        stopped = movie.standby()
        for component in (tv, avr, player):
            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
