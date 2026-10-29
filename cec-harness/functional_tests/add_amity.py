"""Add the second Amity to the original main, then remove/reset it on exit."""

from amity_harness import DUT, TestBed


def test_add_amity():
    main = DUT("amity-dut")
    candidate = DUT("amity-dut-2")
    with TestBed(main, candidate, control_only=(candidate,)):
        with main.add_amity(candidate) as member:
            member.assert_connected()
