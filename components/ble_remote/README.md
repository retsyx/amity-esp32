The BLE remote component shares the NimBLE host initialized by Matter. Start it
after Matter startup; keep `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n`, central,
observer, peripheral, GATT client, and persistent bonding enabled. It never
initializes, stops, or releases the shared controller. Three connection slots
allow commissioning and a remote connection together.

The transport is a generic HID-over-GATT client using ESP-IDF's NimBLE GAP,
GATT, Security Manager, and NVS-backed bond store. It discovers HID services,
Report Maps, Report References, and notification descriptors after establishing
an encrypted bonded link. Keyboard array reports, NKRO bitmap reports,
consumer reports, multiple report IDs, and separate HID services are decoded
from the actual descriptor. A failed or truncated report does not release held
keys; disconnection releases every key. The application receives press/release
events and implements its own key-hold behavior.

`AtomicController` supplies the application key-hold policy independently of the
transport: 500 ms distinguishes short/long gestures at release, and overlapping
HID aliases do not prematurely release the same CEC key. It emits only changes
in the held key; the hub repeats each press after the preceding CEC transmission
attempt finishes, with no repeat timer. The long-activity offset is configurable (default 4). Link loss and event
queue overflow must call `cancel()` before processing synthetic releases, so a
lost remote cannot trigger a pending power/activity gesture or repeat forever.

The installed IDF 6.1 `esp_hid/src/nimble_hidh.c` cannot be used as the transport:
its `read_device_services()` nests Report Map processing inside the Device
Information branch after unconditional `continue` statements, so it never
populates HID reports. This project uses the underlying supported IDF NimBLE
APIs rather than modifying the SDK or copying that implementation.

Pairing starts explicitly from the web UI and automatically selects the first
connectable HID candidate at or above `CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI`
(default −50 dBm, matching Python Amity). The 20-second search skips peers that
reject bonding; advertising alone cannot prove a remote is accepting new bonds.
Weak HID advertisements trigger a bring-closer prompt during the
search; it clears once a qualifying remote is available or pairing starts.
The worker performs security/discovery outside the host scan callback. The API
also retains explicit scan/address pairing for diagnostics and radio fixtures.
Pairing uses Just Works, matching Atomic's available input/output
capabilities. Devices requiring passkey entry are rejected. The component stores
one HID peer's identity separately from NimBLE's encryption keys and reconnects
automatically. Unpair removes just that peer; Matter fabrics and other bonds
remain intact. To clear Atomic's own bond, hold Power and Volume Up for six
seconds, then use Pair Remote from the admin UI.

The Atomic adapter is separate from the generic decoder and transport. Its
mapping follows [the official keymap at commit
11b3704](https://github.com/retsyx/CouchClickerAtomic/blob/11b37040123b897e8d591d63446d87d770a79233/boards/shields/cca/cca.keymap):

| Keyboard usages | Action |
| --- | --- |
| F1–F4 | Activities 0–3 |
| Power | CEC Power |
| Home, Escape, Enter | Root Menu, Back, Select |
| Arrow keys | Direction keys |
| Keyboard Mute, Volume Down, Volume Up | Matching CEC audio controls |

Equivalent consumer-page controls are accepted. No arrow key selects an
activity. Other keyboard keys are ignored by the Atomic adapter.

Host tests exercise the descriptor parser independently of IDF:

```sh
sh tests/ble_test.sh
```

The tests cover Atomic's complete keyboard mapping, 6KRO and NKRO, consumer
arrays, bit offsets, duplicate reports, simultaneous report IDs and maps,
rollover, malformed descriptors, and disconnect releases. Radio pairing and
bond restoration additionally require a real remote or a BLE HID emulator.
`tests/ble_hid_emulator.py` supplies an encrypted BlueZ HID keyboard peripheral
on the Pi using the MIT-licensed `dbus-next==0.2.3` test dependency. Its command
line can inject keyboard/consumer holds after pairing, independently of the
firmware under test. It does not restart BlueZ or remove existing bonds.
`--battery-level 60 --charging yes` changes the running fixture's standard
Battery Service values; charging accepts `yes`, `no`, or `unknown`. Start the
fixture with `--battery-indicate` to exercise status indications instead of
notifications.

`tests/ble_auto_pair_hardware.py --url https://amity-dut.local --credentials
PRIVATE_JSON` exercises automatic search and progress in a real browser,
timeout, rejected bonding, and automatic encrypted HID pairing on an initially
unpaired DUT. The private JSON supplies `admin_password`. It starts/stops the
existing Pi emulator, removes only test-created bonds and restores Pairable.
Use the project-local Playwright environment and coordinate exclusive fixture
use; do not run it against a device with a user remote already paired.

After Matter commissioning and the CEC fixture's eight activities are ready,
the radio tests can be run against the authorized fixture:

```sh
python3 tests/ble_hardware.py --url https://DEVICE_ADDRESS \
  --boot-log test-results/boot-config-fix.log --key-tests --reboot
```

The test reads credentials from the private boot log and records no passwords
or session tokens. It checks scan/pair, battery, keyboard and consumer
subscriptions, all eight activity gestures, power, and bond restoration after
reboot. CEC bus captures are needed separately to confirm the timing and target
of the injected navigation/volume holds. Leave the peer paired for OTA
preservation testing; use `--unpair` only after those checks finish. Atomic or
the emulator must also forget its local bond before a fresh pairing attempt.

`ble_matter_gatt_probe.py` is a separate BlueZ diagnostic for Matter's standard
BTP capability exchange. Coordinate it with the commissioner, since both need
exclusive access to the commissioning connection.

`ble_pi_accesspoint.py` can supply an explicitly authorized, temporary test AP
when the DUT cannot reach the normal network. It uses volatile NetworkManager
profiles, a timed rollback checkpoint, a same-channel virtual AP, and private
project-local credentials and DHCP files. Its default only performs preflight;
`--activate` changes networking. It verifies IPv4 and IPv6 readiness and checks
the AP interface, active connection, and DHCP process throughout the test.
Coordinate any existing Wi-Fi watchdog before activation: a watchdog that
treats AP interfaces as disconnected clients can reset the entire radio. Record
and restore any temporary watchdog pause separately, and verify restoration of
the original uplink after stopping the helper with SIGTERM.

Battery telemetry uses the standard Battery Service (`0x180F`): Battery Level
(`0x2A19`), Battery Level Status (`0x2BED`), and legacy Battery Power State
(`0x2A1A`). Discovery reads current values and subscribes to notifications or
indications. Readable characteristics without a working subscription are polled
every 30 seconds. Charging is never inferred from a rising battery percentage
or from USB power alone. Missing/unknown charging information stays unknown.
Malformed values leave the last valid reading intact. Disconnect discards both
readings; an unpaired device reports the requested default of 100%/not charging.
`tests/battery_status_test.cpp` covers both formats, optional fields, unknown and
absent batteries, percentage limits, and truncated/invalid packets.
