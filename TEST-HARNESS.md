# Test Harness description

## Harness testbed

username: pi
hostname: test2.local

The harness testbed is a Raspberry Pi 5 without a USB console.

## DUTs

| Bench label | ESP32-S3 MAC / USB serial | Amity device ID |
|---|---|---|
| amity-dut | 68:EE:8F:50:2B:44 | dfcddb1bb98d70eca83cd015db66a2ad |
| amity-dut-2 | 44:B1:76:AE:C5:E0 | 7468ba8afeb9ffad49eb57b3ba0a3288 |
| amity-dut-3 | 44:B1:76:AE:C2:AC | fad293fe0f3b513a10e027a20cb3b7c8 |
| amity-dut-4 | 44:B1:76:AE:CF:64 | e453c2c503e55e5f6fe6ab7976c6f1d0 |

## Setup/reset GPIO wiring

| Bench label | DUT GPIO (XIAO pin) | Harness GPIO (BCM/RP1) | Pi physical header pin |
|---|---|---:|---:|
| amity-dut | 4 (D3) | 4 | 7 |
| amity-dut-2 | 4 (D3) | 18 | 12 |
| amity-dut-3 | 4 (D3) | 19 | 35 |
| amity-dut-4 | 4 (D3) | 11 | 23 |

The Pi setup script configures these four GPIOs as inputs with pulls disabled,
releasing each DUT's active-low setup/reset input. It installs and enables
`amity-setup-gpios.service` to repeat that initialization at boot. Rerunning
setup reapplies the idle state; it does not press a button or reset a DUT.

## HDMI-CEC harness GPIO mapping

| Device | Device Tentacle # | Tentacle Front/Back | DUT GPIO | Harness GPIO |
|---|---:|---|---:|---:|
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

### Recreate the virtual devices on the Pi

Run [`tools/cec-harness/setup.sh`](tools/cec-harness/setup.sh) on `test2` with
sudo. It uses the existing GPIO CEC adapters; it recreates the virtual TVs and
players from `devices.tsv`. It installs the follower services, enables them
for boot, restarts all 14 devices and verifies their names, addresses and
running processes. It also installs and runs the setup/reset GPIO initialization
described above. It requires systemd, `cec-ctl` and `cec-follower`
(`v4l-utils`), and `pinctrl` (`raspi-utils`), which are already installed on
`test2`.

To copy the scripts from this checkout and run them on the Pi:

```sh
# On the development computer, from the amity-esp32 checkout:
scp -r tools/cec-harness pi@test2.local:~/
ssh pi@test2.local

# On the Pi:
sudo ~/cec-harness/setup.sh
```

After installation, the same operation is available directly on the Pi,
without the source checkout:

```sh
sudo /usr/local/sbin/amity-cec-harness-setup
```

The services automatically recreate the virtual devices after a power cycle;
normally no manual command is needed. Rerun the command above to restore the
configuration after a test changes it or to restart all virtual devices.
Rerunning is safe and returns them to their initial standby state. The script
reports success only when every configured follower is running with the
expected CEC identity. DUT settings are unaffected.
Both the source script and the installed command were run successfully on
`test2`, recreating and verifying all 14 virtual devices.

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



