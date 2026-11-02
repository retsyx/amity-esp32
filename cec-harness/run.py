#!/usr/bin/env python3
"""Run Amity functional tests on the Pi CEC harness."""

from importlib import import_module

from amity_harness.runner import main
from functional_tests.activity import (test_activity, test_activity_switch_input,
                                       test_activity_keys, test_activity_keys_secondary)
from functional_tests.add_amity import test_add_amity
from functional_tests.hdmi_scan import test_hdmi_scan
from functional_tests.source_theft import test_source_theft
from functional_tests.consortium import (
    test_consortium, test_consortium_three_duts, test_consortium_four_duts)

test_dual_display = import_module("functional_tests.dual-display").test_dual_display
test_source_theft_isolated = import_module("functional_tests.source-theft").test_source_theft_isolated

if __name__ == "__main__":
    raise SystemExit(main({"hdmi_scan": test_hdmi_scan,
                           "activity": (test_activity, test_activity_switch_input,
                                        test_activity_keys, test_activity_keys_secondary),
                           "source_theft": test_source_theft, "add_amity": test_add_amity,
                           "consortium": test_consortium,
                           "consortium_three_duts": test_consortium_three_duts,
                           "consortium_four_duts": test_consortium_four_duts,
                           "dual_display": test_dual_display,
                           "source_theft_isolated": test_source_theft_isolated}))
