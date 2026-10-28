#!/bin/sh
# Prepare setup/reset inputs and recreate the Pi's virtual CEC devices.
set -eu
if [ "$#" -ne 0 ]; then
    echo "Usage: sudo $0" >&2
    echo "Prepare setup/reset GPIOs and install, enable and restart all virtual CEC devices." >&2
    case "$1" in -h|--help) exit 0 ;; *) exit 2 ;; esac
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script with sudo." >&2
    exit 1
fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$source_dir/devices.tsv" ]; then
    profiles=$source_dir/devices.tsv
    wrapper=$source_dir/device.sh
    service=$source_dir/amity-cec@.service
    setup_gpio_wrapper=$source_dir/setup-gpios.sh
    setup_gpio_service=$source_dir/amity-setup-gpios.service
else
    # The installed command remains usable without the original checkout.
    profiles=/usr/local/share/amity-cec-harness/devices.tsv
    wrapper=/usr/local/lib/amity-cec-harness/device.sh
    service=/etc/systemd/system/amity-cec@.service
    setup_gpio_wrapper=/usr/local/lib/amity-cec-harness/setup-gpios.sh
    setup_gpio_service=/etc/systemd/system/amity-setup-gpios.service
fi
for command in /usr/bin/cec-ctl /usr/bin/cec-follower /usr/bin/stdbuf; do
    if [ ! -x "$command" ]; then
        echo "Missing $command; install v4l-utils and coreutils first." >&2
        exit 1
    fi
done
command -v systemctl >/dev/null
command -v pinctrl >/dev/null || {
    echo "Missing pinctrl; install Raspberry Pi's raspi-utils first." >&2
    exit 1
}
id pi >/dev/null
getent group video >/dev/null
for file in "$profiles" "$wrapper" "$service" "$setup_gpio_wrapper" "$setup_gpio_service"; do
    if [ ! -r "$file" ]; then
        echo "Missing harness source file: $file" >&2
        exit 1
    fi
done
# Validate the whole profile before changing any installed files or services.
gpios=$(awk '
    /^#/ || NF == 0 {next}
    NF != 4 || $1 !~ /^[0-9]+$/ || seen_gpio[$1]++ ||
    $4 !~ /^[A-Za-z0-9_-]+$/ || length($4) > 14 || seen_name[$4]++ ||
    !(($2 == "tv" && $3 == "0.0.0.0") ||
      ($2 == "playback" && $3 == "1.0.0.0")) {
        print "Invalid or duplicate virtual CEC profile at line " NR > "/dev/stderr"
        failed=1; exit 1
    }
    {print $1; count++}
    END {if (failed || !count) exit 1}
' "$profiles")
set --
for gpio in $gpios; do
    case "$gpio" in 4|18|19|11)
        echo "CEC GPIO $gpio overlaps a DUT setup/reset connection." >&2
        exit 1
    esac
    if ! /usr/bin/cec-ctl -D cec-gpio -a "cec@$gpio" >/dev/null; then
        echo "Existing GPIO CEC adapter cec@$gpio is unavailable." >&2
        exit 1
    fi
    set -- "$@" "amity-cec@$gpio.service"
done
install -d -m 0755 /usr/local/lib/amity-cec-harness /usr/local/share/amity-cec-harness /usr/local/sbin
copy_file() {
    # Also permits rerunning the installed command with its files as inputs.
    if ! cmp -s "$1" "$2"; then install -m "$3" "$1" "$2"; fi
    chmod "$3" "$2"
}
copy_file "$profiles" /usr/local/share/amity-cec-harness/devices.tsv 0644
copy_file "$wrapper" /usr/local/lib/amity-cec-harness/device.sh 0755
copy_file "$service" /etc/systemd/system/amity-cec@.service 0644
copy_file "$setup_gpio_wrapper" /usr/local/lib/amity-cec-harness/setup-gpios.sh 0755
copy_file "$setup_gpio_service" /etc/systemd/system/amity-setup-gpios.service 0644
copy_file "$source_dir/$(basename -- "$0")" /usr/local/sbin/amity-cec-harness-setup 0755
systemctl daemon-reload
systemctl enable amity-setup-gpios.service "$@"
systemctl restart amity-setup-gpios.service
systemctl is-active --quiet amity-setup-gpios.service
systemctl is-enabled --quiet amity-setup-gpios.service
systemctl restart "$@"

# Type=simple can be active while the wrapper still claims its CEC address.
# Wait for the actual follower and verify all configured device identities.
attempt=0
while [ "$attempt" -lt 30 ]; do
    pending=0
    while read -r gpio role physical name; do
        case "$gpio" in ''|'#'*) continue ;; esac
        unit=amity-cec@$gpio.service
        pid=$(systemctl show "$unit" -p MainPID --value)
        case "$role" in tv) logical=0 ;; playback) logical=4 ;; esac
        info=$(/usr/bin/cec-ctl -D cec-gpio -a "cec@$gpio" 2>/dev/null) || info=
        if ! systemctl is-active --quiet "$unit" ||
           ! systemctl is-enabled --quiet "$unit" ||
           [ "$(cat "/proc/$pid/comm" 2>/dev/null || true)" != cec-follower ] ||
           ! printf '%s\n' "$info" | awk -v name="$name" -v pa="$physical" -v la="$logical" '
               /OSD Name[[:space:]]*:/ {osd=($NF == "\047" name "\047")}
               /Physical Address[[:space:]]*:/ {physical=($NF == pa)}
               /Logical Address[[:space:]]*:/ {logical=($4 == la)}
               END {exit !(osd && physical && logical)}'; then
            pending=$((pending + 1))
        fi
    done < /usr/local/share/amity-cec-harness/devices.tsv
    if [ "$pending" -eq 0 ]; then
        echo "Setup/reset GPIOs 4, 18, 19 and 11 are inputs with pulls disabled, enabled for boot."
        echo "All $# virtual HDMI-CEC devices are running and enabled for boot."
        exit 0
    fi
    attempt=$((attempt + 1))
    sleep 1
done
echo "$pending virtual devices did not become ready. Inspect: systemctl status 'amity-cec@*.service'" >&2
exit 1
