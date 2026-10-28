#!/bin/sh
# Release each DUT's active-low GPIO4; never drive a setup/reset pulse here.
set -eu
for gpio in 4 18 19 11; do
    pinctrl set "$gpio" ip pn
    pinctrl get "$gpio"
done
