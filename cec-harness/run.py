#!/usr/bin/env python3
"""Run Amity functional tests on the Pi CEC harness."""

from amity_harness.runner import main
from functional_tests.activity import test_activity, test_activity_switch_input
from functional_tests.add_amity import test_add_amity
from functional_tests.hdmi_scan import test_hdmi_scan
from functional_tests.source_theft import test_source_theft
from functional_tests.consortium import (
    test_consortium, test_consortium_three_duts, test_consortium_four_duts)

if __name__ == "__main__":
    raise SystemExit(main({"hdmi_scan": test_hdmi_scan, "activity": (test_activity, test_activity_switch_input),
                           "source_theft": test_source_theft, "add_amity": test_add_amity,
                           "consortium": test_consortium,
                           "consortium_three_duts": test_consortium_three_duts,
                           "consortium_four_duts": test_consortium_four_duts}))
