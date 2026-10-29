"""Activity power transitions and explicit switch-input selection."""

from amity_harness import PowerState
from functional_tests.hdmi_scan import scanned_devices


def test_activity():
    with scanned_devices() as (dut, tv, avr, player):
        components = (tv, avr, player)
        with dut.create_activity("Functional activity", display=tv, source=player, audio=avr) as activity:
            for component in components:
                component.assert_power(PowerState.STANDBY)

            started = activity.start()
            for component in components:
                component.wait_for_power(PowerState.ON, since=started)

            stopped = activity.standby()
            for component in components:
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")


def test_activity_switch_input():
    with scanned_devices() as (dut, tv, avr, player):
        # No CEC source: select the AVR's requested input for a non-CEC source.
        with dut.create_activity("Functional switch input", display=tv, source=None, audio=avr,
                                 switch_device=avr, switch_input=4) as activity:
            started = activity.start()
            for component in (tv, avr):
                component.wait_for_power(PowerState.ON, since=started)
            avr.wait_for_switch_input(4, since=started)
            player.assert_power(PowerState.STANDBY)
            activity.assert_active()
