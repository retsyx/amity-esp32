# Test Harness description

## Harness testbed

username: pi
hostname: test2.local

The harness testbed is a Raspberry Pi 5 without a USB console.

## DUTs

| Bench label | ESP32-S3 MAC / USB serial | Amity device ID | Administration hostname |
|---|---|---|---|
| amity-dut | 68:EE:8F:50:2B:44 | dfcddb1bb98d70eca83cd015db66a2ad | amity |
| amity-dut-2 | 44:B1:76:AE:C5:E0 | 7468ba8afeb9ffad49eb57b3ba0a3288 | |
| amity-dut-3 | 44:B1:76:AE:C2:AC | fad293fe0f3b513a10e027a20cb3b7c8 | |
| amity-dut-4 | 44:B1:76:AE:CF:64 | e453c2c503e55e5f6fe6ab7976c6f1d0 | |

The administration hostname is the factory hostname used after reset; the bench
label identifies physical wiring. Secondary boards are headless members and do
not expose their own administration UI.

## Setup/reset GPIO wiring

| Bench label | DUT GPIO (XIAO pin) | Harness GPIO (BCM/RP1) | Pi physical header pin |
|---|---|---:|---:|
| amity-dut | 4 (D3) | 4 | 7 |
| amity-dut-2 | 4 (D3) | 18 | 12 |
| amity-dut-3 | 4 (D3) | 19 | 35 |
| amity-dut-4 | 4 (D3) | 11 | 23 |

These are active-low setup/reset inputs. The Python framework releases them as
inputs with pulls disabled and drives a low pulse only for an explicit DUT
button press. The framework reads this table and the CEC mapping table below;
update the tables when bench wiring changes.

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
