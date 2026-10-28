# Test Harness description

## Harness testbed

username: pi
hostname: test2.local

The harness testbed is a Raspberry Pi 5 without a USB console.

## DUTs

All four DUTs run four-tentacle Amity 0.1.84 with normal encrypted admission,
installed on 28 October 2026. This application-only update preserved their
settings, factory Matter/admin credentials and device IDs from the earlier
clean 0.1.82 installation. `amity-dut` currently uses hostname `amity-dut`;
the other three retain the default hostname `amity`.
The names below are physical bench labels; use the USB MAC to match each board
to its setup card and device ID.

Uncommissioned boards now open Matter setup automatically at boot for five
minutes, without a setup button. A board with a Matter fabric does not reopen
it on reboot. Already enrolled headless Amity members remain excluded.
`amity-dut` completed Apple Home Matter commissioning on 28 October and started
its original-main services; the other DUTs remain available for initial setup.

| Bench label | ESP32-S3 MAC / USB serial | Amity device ID |
|---|---|---|
| amity-dut | 68:EE:8F:50:2B:44 | dfcddb1bb98d70eca83cd015db66a2ad |
| amity-dut-2 | 44:B1:76:AE:C5:E0 | 7468ba8afeb9ffad49eb57b3ba0a3288 |
| amity-dut-3 | 44:B1:76:AE:C2:AC | fad293fe0f3b513a10e027a20cb3b7c8 |
| amity-dut-4 | 44:B1:76:AE:CF:64 | e453c2c503e55e5f6fe6ab7976c6f1d0 |

## HDMI-CEC harness GPIO mapping

Harness GPIO numbers are BCM/RP1 line offsets. DUT GPIO numbers are ESP32-S3
GPIO numbers, rather than XIAO D-pin labels.

| Device | Device Tentacle # | Tentacle Front/Back | DUT GPIO | Harness GPIO |
|---|---|---|---|---|
| amity-dut   | 0 | front | 1 | 23 |
| amity-dut   | 0 | back | 2 | 24 |
| amity-dut-2 | 0 | front | 1 | 17 |
| amity-dut-2 | 0 | back | 2 | 27 |
| amity-dut-3 | 0 | front | 1 | 22 |
| amity-dut-3 | 0 | back | 2 | 25 |
| amity-dut-4 | 0 | front | 1 | 12 |
| amity-dut-4 | 0 | back | 2 | 13 |
| amity-dut-4 | 1 | front | 5 | 5 |
| amity-dut-4 | 1 | back | 6 | 6 |
| amity-dut-4 | 2 | front | 7 | 20 |
| amity-dut-4 | 2 | back | 8 | 21 |
| amity-dut-4 | 3 | front | 9 | 16 |
| amity-dut-4 | 3 | back | 44 | 26 |

## Virtual HDMI-CEC devices — 28 October 2026

All 14 GPIO CEC endpoints have a continuously running virtual device: a TV on
each front wire and a playback source on each back wire. TVs use logical
address 0 and physical address `0.0.0.0`; players use logical address 4 and
physical address `1.0.0.0`. Each wire is isolated, so these addresses repeat
across wires. All devices use CEC 2.0 and vendor ID `0x000001`.

| Bench label | HDMI adapter | Side | Harness GPIO | Current Linux device | Virtual OSD name |
|---|---:|---|---:|---|---|
| amity-dut | 0 | front | 23 | `/dev/cec13` | `DUT1-TV` |
| amity-dut | 0 | back | 24 | `/dev/cec12` | `DUT1-Player` |
| amity-dut-2 | 0 | front | 17 | `/dev/cec11` | `DUT2-TV` |
| amity-dut-2 | 0 | back | 27 | `/dev/cec10` | `DUT2-Player` |
| amity-dut-3 | 0 | front | 22 | `/dev/cec9` | `DUT3-TV` |
| amity-dut-3 | 0 | back | 25 | `/dev/cec8` | `DUT3-Player` |
| amity-dut-4 | 0 | front | 12 | `/dev/cec5` | `DUT4-A0-TV` |
| amity-dut-4 | 0 | back | 13 | `/dev/cec4` | `DUT4-A0-Player` |
| amity-dut-4 | 1 | front | 5 | `/dev/cec7` | `DUT4-A1-TV` |
| amity-dut-4 | 1 | back | 6 | `/dev/cec6` | `DUT4-A1-Player` |
| amity-dut-4 | 2 | front | 20 | `/dev/cec1` | `DUT4-A2-TV` |
| amity-dut-4 | 2 | back | 21 | `/dev/cec0` | `DUT4-A2-Player` |
| amity-dut-4 | 3 | front | 16 | `/dev/cec3` | `DUT4-A3-TV` |
| amity-dut-4 | 3 | back | 26 | `/dev/cec2` | `DUT4-A3-Player` |

Each GPIO has an enabled systemd service, `amity-cec@<GPIO>.service`, which
configures the address/name with `cec-ctl`, then runs `cec-follower` 1.30.1.
The follower handles power, remote-control and other CEC interactions; it
starts in standby and returns to standby when the service restarts. Services
restart on failure and start automatically with the harness. They select the
stable `cec-gpio` / `cec@<GPIO>` adapter identity, since `/dev/cecN` numbering
can change after a reboot or driver rebind.

The source profiles, wrapper and service template are in
[`tools/cec-harness`](tools/cec-harness). On `test2`, they are installed as
`/usr/local/share/amity-cec-harness/devices.tsv`,
`/usr/local/lib/amity-cec-harness/device.sh` and
`/etc/systemd/system/amity-cec@.service` respectively.

For example, inspect or restart the first TV with:

```sh
ssh pi@test2.local 'systemctl status amity-cec@23.service --no-pager'
ssh pi@test2.local 'cec-ctl -D cec-gpio -a cec@23'
ssh pi@test2.local 'sudo journalctl -u amity-cec@23.service -f'
ssh pi@test2.local 'sudo systemctl restart amity-cec@23.service'
```

Stop the affected services before tests that clear/reassign adapter addresses
or manipulate GPIO drivers, then restart them to restore these devices.
`tests/cec_hardware.py` is one such test.

Verification confirmed all 14 followers running and enabled, with the exact
OSD names, physical addresses and logical addresses above, including after a
service restart. This establishes harness configuration and process health;
it does not claim a DUT-originated request/reply test on every wire. End-to-end
DUT discovery and follower power-response checks remain unverified. No DUT
settings were changed.
[Deployment report](test-results/cec-virtual-devices-2026-10-28/deployment.json).

## Electrical verification — 27 October 2026

The table above was measured directly with open-drain GPIO low/release tests:
three cycles in each direction for all 14 wires, 84 cycles total. Every pulse
reached exactly one corresponding line; the other 13 tested lines remained
high. The original table matched the first three DUTs

The test temporarily paused both ESP32 cores through USB JTAG and released the
Pi's CEC GPIO drivers. It restored the GPIO registers and original Pi driver
assignments/settings, then restarted the DUT firmware without writing flash.
Detailed expected/measured mappings and raw samples are saved in
[the verification report](test-results/gpio-mapping-2026-10-27/VERIFICATION.md).

## Current setup cards

The four-tentacle build uses front/back GPIO pairs `1/2`, `5/6`, `7/8`,
and `9/44`, with setup/reset on GPIO4.

[View or print the four setup cards](test-results/admission-0.1.83-deployment-2026-10-27/index.html)
or [download the private card archive](test-results/admission-0.1.83-deployment-2026-10-27/amity-0.1.83-setup-cards.zip).
The cards include private credentials and are labelled by USB MAC.

Normal Add needs no setup code: open **Amity devices → Add** on the main,
hold the new board's **FW RESET** for three seconds and release before ten,
then choose it, enter a name and add it. The Matter QR/manual code remains
available for first-main setup and the optional code-verified build.

All four updates passed exact image/boot identity, retained-card/device-ID and
actual SDK admission security checks. Each boot opened its Matter window and
reported CHIPoBLE advertising started. Flash ranges outside the selected
application partition and the OTA selection metadata were verified unchanged
before reboot. Home discovery and completed commissioning were observed on
`amity-dut`; this does not claim Home commissioning on the other three.
[Deployment verification](test-results/matter-auto-boot-2026-10-28/deployment.json).
