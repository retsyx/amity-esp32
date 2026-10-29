#!/usr/bin/env python3
"""Run the single Amity HDMI scan functional test on the Pi harness."""

from amity_harness.runner import main
from functional_tests.hdmi_scan import test_hdmi_scan

if __name__ == "__main__":
    raise SystemExit(main(test_hdmi_scan))
