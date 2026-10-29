#!/usr/bin/env python3
"""Run Amity functional tests on the Pi CEC harness."""

from amity_harness.runner import main
from functional_tests.activity import test_activity
from functional_tests.hdmi_scan import test_hdmi_scan
from functional_tests.source_theft import test_source_theft

if __name__ == "__main__":
    raise SystemExit(main({"hdmi_scan": test_hdmi_scan, "activity": test_activity,
                           "source_theft": test_source_theft}))
