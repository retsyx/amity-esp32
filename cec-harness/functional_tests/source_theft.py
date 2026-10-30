"""An unselected player cannot steal the running activity's HDMI source."""

from amity_harness import PowerState
from functional_tests.hdmi_scan import scanned_devices


def test_source_theft():
    with scanned_devices(extra_player=True) as (dut, tv, avr, player, other):
        # Create both while idle; close the running activity first on failure.
        with dut.create_activity("Other player", display=tv, source=other, audio=avr):
            with dut.create_activity("Selected player", display=tv, source=player, audio=avr) as activity:
                for component in (tv, avr, player, other):
                    component.assert_power(PowerState.STANDBY)

                started = activity.start()
                for component in (tv, avr, player):
                    component.wait_for_power(PowerState.ON, since=started)
                other.assert_power(PowerState.STANDBY)
                avr.wait_for_input(player, since=started)
                activity.assert_active()

                stolen = other.checkpoint()
                announcement = other.activate()
                assert announcement.ok
                other.wait_for_standby_after(announcement, within=1)
                avr.wait_for_input(player, since=stolen, after=announcement, within=1)
                for component in (tv, avr, player):
                    component.assert_power(PowerState.ON)
                assert player.active and not other.active
                activity.assert_active()
