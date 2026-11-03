"""Remove a member while saved activities still reference its equipment."""

from contextlib import ExitStack

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.removal import MembershipContinuity
from amity_harness.qualified_names import observe_no_control


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


def _exercise_owner(main, activity, equipment, scan, *inactive):
    activity.refresh_scan(scan)
    for component in (*equipment, *inactive):
        component.assert_power(PowerState.STANDBY)
    with observe_no_control(scan, *inactive) as observation:
        started = activity.start()
        for component in equipment:
            component.wait_for_power(PowerState.ON, since=started)
        equipment[1].wait_for_input(equipment[2], since=started)
        main.press_key(Key.VOLUME_UP).assert_received_by(equipment[1])
        main.press_key(Key.RIGHT).assert_received_by(equipment[2])
        activity.assert_active()
        for component in inactive:
            component.assert_power(PowerState.STANDBY)
        observation.assert_clear()
    stopped = activity.standby()
    for component in equipment:
        component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
    for component in inactive:
        component.assert_power(PowerState.STANDBY)


def test_removal_replacement():
    main, second, third, fourth = (
        DUT("amity-dut"), DUT("amity-dut-2"), DUT("amity-dut-3"), DUT("amity-dut-4"))
    local, old, kept, new = (_equipment(owner, "Same") for owner in (main, second, third, fourth))
    components = (*local, *old, *kept, *new)
    with TestBed(main, second, third, fourth, *components, members=(second, third, fourth)):
        with ExitStack() as resources:
            removed = resources.enter_context(main.add_amity(second))
            removed.assert_connected()
            survivor = resources.enter_context(main.add_amity(third))
            survivor.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*local, *old, *kept)
            for component in (*local, *old, *kept):
                scan.assert_on(component, component.endpoint)

            keeper = resources.enter_context(main.create_activity(
                "Replacement keeper", display=kept[0], audio=kept[1], source=kept[2]))
            affected = resources.enter_context(main.create_activity(
                "Removed owner", display=old[0], audio=old[1], source=old[2],
                switch_device=old[1], switch_input=1))
            continuity = MembershipContinuity(survivor, keeper)
            removed.review_removal(affected, preserve=(survivor,)).confirm()

            scan = main.scan_hdmi()
            scan.assert_contains_only(*local, *kept)
            for component in (*local, *kept):
                scan.assert_on(component, component.endpoint)
            continuity.assert_preserved(survivor, removed=(removed,))
            _exercise_owner(main, keeper, kept, scan, *local)

            added = resources.enter_context(main.add_amity(fourth))
            added.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*local, *kept, *new)
            for component in (*local, *kept, *new):
                scan.assert_on(component, component.endpoint)
            continuity.assert_preserved(survivor, added, removed=(removed,))

            newcomer = resources.enter_context(main.create_activity(
                "Replacement owner", display=new[0], audio=new[1], source=new[2],
                switch_device=new[1], switch_input=1))
            _exercise_owner(main, keeper, kept, scan, *local, *new)
            _exercise_owner(main, newcomer, new, scan, *local, *kept)

            # Keep the survivor's saved activity throughout the replacement's
            # own removal; ordinary fixture cleanup runs after these checks.
            added.review_removal(newcomer).confirm()
            continuity.assert_preserved(survivor, removed=(removed, added))
