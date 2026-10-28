#!/bin/sh
# Configure one isolated test2 CEC wire, then emulate its device continuously.
set -eu
gpio=${1:?GPIO number required}
case "$gpio" in *[!0-9]*|'') exit 2 ;; esac
profile=$(awk -v gpio="$gpio" '$1 == gpio {print $2, $3, $4; found++} END {if(found != 1) exit 1}' /usr/local/share/amity-cec-harness/devices.tsv)
read -r role physical name <<EOF
$profile
EOF
case "$role" in tv) expected=0 ;; playback) expected=4 ;; *) exit 2 ;; esac
# Device numbers can change after a driver rebind or reboot. Match the GPIO
# adapter's stable name instead of assuming /dev/cecN identifies a wire.
adapter=cec@$gpio
/usr/bin/cec-ctl -D cec-gpio -a "$adapter" --clear "--$role" \
    --phys-addr "$physical" --osd-name "$name" --vendor-id 0x000001 \
    --no-rc-passthrough
actual=$(/usr/bin/cec-ctl -D cec-gpio -a "$adapter" --skip-info --logical-address)
if [ "$actual" != "$expected" ]; then
    echo "Could not claim logical address $expected on $adapter (got $actual)" >&2
    exit 1
fi
exec /usr/bin/stdbuf -oL -eL /usr/bin/cec-follower -D cec-gpio -a "$adapter" --exclusive \
    --wall-clock --show-msgs --show-state --standby
