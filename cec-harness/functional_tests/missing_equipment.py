"""Saved equipment survives disappearance, lookup cooldown and passive return."""

from amity_harness import Key, PowerState
from amity_harness.recovery import LookupScans, assert_returned, assert_switch_commands
from functional_tests.hdmi_scan import scanned_devices


def test_missing_equipment():
    with scanned_devices() as (dut, tv, avr, player):
        with dut.create_activity("Functional missing source", display=tv, source=player, audio=avr,
                                 switch_device=avr, switch_input=4) as activity:
            player.power_off()
            missing = dut.scan_hdmi()
            missing.assert_contains_only(tv, avr)
            missing.assert_on(tv, tv.endpoint)
            missing.assert_on(avr, avr.endpoint)

            with LookupScans(missing, tv, avr) as lookups:
                # Actual starts must retain the saved source and use the
                # configured numeric fallback while its name is unresolved.
                for _ in range(2):
                    started = activity.start()
                    for component in (tv, avr):
                        component.wait_for_power(PowerState.ON, since=started)
                    avr.wait_for_switch_input(4, since=started)
                    player.assert_power(PowerState.OFF)
                    activity.assert_active()
                    lookups.assert_no_scan()
                    assert_switch_commands(avr, 4, since=started, count=1)

                lookups.wait_for_cooldown()
                # Expiry did nothing. A new real communication miss now scans
                # only the source's back side and completes its own fallback.
                retried = activity.start()
                avr.wait_for_switch_input(4, since=retried)
                activity.assert_active()
                lookups.assert_scan_on(avr.endpoint)
                assert_switch_commands(avr, 4, since=retried, count=1)

            with LookupScans(missing, tv, avr) as announcements:
                returned = player.checkpoint()
                player.standby()
                assert_returned(missing, player, since=returned)

                restored = activity.start()
                for component in (tv, avr, player):
                    component.wait_for_power(PowerState.ON, since=restored)
                avr.wait_for_input(player, since=restored)
                activity.assert_active()
                dut.press_key(Key.SELECT).assert_received_by(player)
                announcements.assert_no_scan()
                assert_switch_commands(avr, 4, since=restored, count=0)

            stopped = activity.standby()
            for component in (tv, avr, player):
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
