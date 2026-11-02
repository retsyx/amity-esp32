"""Remove a member while saved activities still reference its equipment."""

from contextlib import ExitStack

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState


def _equipment(dut, name):
    connection = dut.tentacle(0)
    tv = TvComponent(connection.front, f"{name} TV")
    avr = AvrComponent(connection.back, f"{name} AVR")
    player = PlaybackComponent(avr, f"{name} Player", port=1)
    return tv, avr, player


def test_removal_cascade():
    main, secondary = DUT("amity-dut"), DUT("amity-dut-2")
    local, remote = _equipment(main, "Keep"), _equipment(secondary, "Remove")
    components = (*local, *remote)

    with TestBed(main, secondary, *components, members=(secondary,)):
        with main.add_amity(secondary) as member:
            member.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains(*components)
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)

            with ExitStack() as activities:
                keeper = activities.enter_context(main.create_activity(
                    "Removal keeper", display=local[0], audio=local[1], source=local[2]))
                source = activities.enter_context(main.create_activity(
                    "Remove member source", display=local[0], audio=local[1], source=remote[2]))
                display = activities.enter_context(main.create_activity(
                    "Remove member display", display=remote[0], audio=local[1], source=local[2]))
                audio = activities.enter_context(main.create_activity(
                    "Remove member audio", display=local[0], audio=remote[1], source=local[2]))
                switch = activities.enter_context(main.create_activity(
                    "Remove member switch", display=local[0], audio=local[1], source=None,
                    switch_device=remote[1], switch_input=3))
                affected = (source, display, audio, switch)

                review = member.review_removal(*affected)
                activities.enter_context(main.create_activity(
                    "Removal second keeper", display=local[0], audio=local[1], source=local[2]))
                review.assert_stale_rejected()
                member.review_removal(*affected).confirm()

                started = keeper.start()
                for component in local:
                    component.wait_for_power(PowerState.ON, since=started)
                local[1].wait_for_input(local[2], since=started)
                keeper.assert_active()
                stopped = keeper.standby()
                for component in local:
                    component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
