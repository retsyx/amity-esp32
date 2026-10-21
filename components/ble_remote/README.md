The BLE remote component shares the NimBLE host initialized by Matter. Start it
after Matter startup; keep `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n`, central,
observer, peripheral, GATT client, and persistent bonding enabled. It never
initializes, stops, or releases the shared controller. Three connection slots
allow commissioning and a remote connection together.

The Atomic transport is a generic HID-over-GATT client using ESP-IDF's NimBLE GAP,
GATT, Security Manager, and NVS-backed bond store. It discovers HID services,
Report Maps, Report References, and notification descriptors after establishing
an encrypted bonded link. Keyboard array reports, NKRO bitmap reports,
consumer reports, multiple report IDs, and separate HID services are decoded
from the actual descriptor. A failed or truncated report does not release held
keys; disconnection releases every key. The application receives press/release
events and implements its own key-hold behavior.

`AtomicInputAdapter` translates HID usages into common logical button events;
`SiriInputAdapter` translates Apple's button reports and recognizes touch swipes.
Both feed one `RemoteInputController` in the application. It owns independent
logical-button holds, aliases, 500 ms short/long gestures, power and activity
shortcuts, and cancellation. Siri reports are processed as batches so simultaneous button
changes preserve Power shortcuts. The binding profiles retain Atomic's dedicated
activity buttons and Siri's standby selectors and Power chords.

Each queued report captures its timestamp, activity generation and reserved
target. Activity changes quarantine held buttons in the common controller until
release; stale queued reports cannot resume a key on the new source. Presses
also carry their generation to the hub for validation when dequeued. Each
`RemoteCommand::Press` and `Release` identifies its logical CEC key. Physical
aliases, such as keyboard and consumer Volume Up usages, share one logical
press until the last alias releases. Duplicate reports do not create presses.
Ordinary keys remain held independently: Volume Up down, Right down, Right up
emits `Press(41)`, `Press(04)`, `Release(04)`; Volume Up continues until its own
release. No release/repress of Volume Up occurs around the Right interaction.

This matches Python Amity's
[per-key input state](https://github.com/retsyx/amity/blob/main/main.py#L144-L208)
and [independent press coroutines](https://github.com/retsyx/amity/blob/main/main.py#L286-L320).
The hub submits each new key without waiting for another key's CEC ACK.
Completion permits only that key's next press, with at most one outstanding
attempt per held key and no repeat timer or queued repeat backlog. Key-up
removes only its own hold; already admitted CEC frames finish. The hub sends
operandless CEC release only when all held keys at that receiving device end.
The complete routing, failure, release-capacity and overload contract is in
[`amity_hub/README.md`](../amity_hub/README.md).

Activity changes suppress every old held button until physical release.
Python's [`wait_for_release`](https://github.com/retsyx/amity/blob/main/main.py#L128-L168)
also gates **new** presses while the selection gesture is being released.
The input policy starts with this gate armed. Successful activity selections,
including same-target selections and standby requests from the remote or an
external controller, arm it again. Configuration replacement does too. A
rejected request does not arm it. Long Power's `fix_current_activity` repair
follows Python's separate path and does not arm the selection gate.

While waiting, physical button state and releases are still consumed, but new
forwarded CEC presses, activity-macro recognition and finite swipe commands are
discarded. Releases still handle Power and standby activity selection, as in
Python. The gate clears when the last physical button/alias is released, or
after a one-second fallback updated by the input task without blocking. A
swallowed hold stays suppressed until its own release even after the timeout;
it cannot replay into the selected activity. New presses after expiry work
normally. A report processes releases before fresh presses, so its final old
release can clear the gate before a new button goes down. Action-report
suppression still prevents a new press from leaking out of the same report
that selected an activity.

`HubService::activity_selection()` reports accepted selections separately from
`activity_generation`, because same-target selection need not change the latter.
The input task observes it through `RemoteInputController::activity_selected()`.
The originating remote action already arms its gate; observing that same
selection does not re-arm a gate cleared by its final release. This gate only
filters input admission. It never waits for CEC ACKs or stalls other hub work.

Link loss and queue overflow cancel all logical holds and pending gestures
before synthetic releases, preventing a lost remote from completing a pending
power/activity gesture or repeating forever. Siri Power chords retain batch
suppression so a simultaneous selector does not generate a stray navigation
press.

The installed IDF 6.1 `esp_hid/src/nimble_hidh.c` cannot be used as the transport:
its `read_device_services()` nests Report Map processing inside the Device
Information branch after unconditional `continue` statements, so it never
populates HID reports. This project uses the underlying supported IDF NimBLE
APIs rather than modifying the SDK or copying that implementation.

Pairing starts explicitly from the web UI and automatically selects the first
connectable HID or Gen 2/3 Siri Remote candidate at or above `CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI`
(default −50 dBm, matching Python Amity). The 20-second search skips peers that
reject bonding; advertising alone cannot prove a remote is accepting new bonds.
Weak supported-remote advertisements trigger a bring-closer prompt during the
search; it clears once a qualifying remote is available or pairing starts.
The worker performs security/discovery outside the host scan callback. The API
also retains explicit scan/address pairing for diagnostics and radio fixtures.
Pairing uses Just Works, matching Atomic's available input/output
capabilities. Devices requiring passkey entry are rejected. The component stores
one remote peer's identity separately from NimBLE's encryption keys. With a saved
peer, it listens asynchronously for that peer's connectable advertisements and
initiates a connection only when one arrives. An asleep or absent remote does
not cause repeated connection attempts or discovery timeouts. The status/API
`waiting` flag identifies this background wait; Advanced → Diagnostics shows
“Waiting for remote”. `connecting` identifies actual connection, security and
service discovery, and real errors remain visible. Unpair cancels the wait
promptly and removes just that peer; Matter fabrics and other bonds
remain intact. To clear Atomic's own bond, hold Power and Volume Up for six
seconds, then use Pair Remote from the admin UI.

Unpairing is asynchronous. The status/API `unpairing` flag stays true from
command acceptance until bond storage cleanup and any active link termination
finish. Scan and pair requests are rejected during this time. The web button
stays disabled, and both its label and the status show “Unpairing...”.

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

Siri Remote Gen 2 (`0x0314`) and Gen 3 (`0x0315`) use a separate Apple report
adapter, following Python Amity's `pair_tool.py`, `remote.py`, `remote_adapter.py`
and `gestures.py`. Manufacturer data detects these models even without a HID
advertisement. Device Information PNP IDs identify the model again on every
connection, including reconnects. Gen 1 and Gen 1.5 are excluded even when they
advertise HID. The firmware revision selects Apple's input/touch handles,
notification writes, initialization commands and charging codes; revisions
before and after `0x0083` are supported. The ATT MTU is negotiated to 527.

While in standby, Siri's Select, Up, Right, Down and Left clicks select activities
0–4 respectively, on release. Holding for at least 500 ms selects activities
5–9. With an activity active these buttons navigate normally. Siri supports the
fixed Power-button macros from Python Amity's
[`hub.macros` defaults and long-press map](https://github.com/retsyx/amity/blob/main/main.py#L61-L90):

| Siri macro | Selector held less than 500 ms | Selector held at least 500 ms |
| --- | --- | --- |
| Power + Select | Activity 0 | Activity 5 |
| Power + Up | Activity 1 | Activity 6 |
| Power + Right | Activity 2 | Activity 7 |
| Power + Down | Activity 3 | Activity 8 |
| Power + Left | Activity 4 | Activity 9 |

These macros are available only on the Siri profile and are not configurable.
Atomic retains its dedicated F1–F4 activity shortcuts; combining its Power and
direction/Select buttons does not form an activity macro. Indices in the table
are zero-based; selecting an unconfigured activity is rejected.

Either macro button may be pressed first. The first release of Power or the
chosen selector executes the macro once; releasing an unrelated key does not.
The duration is measured from the selector's original press to that first
macro-button release, regardless of how long Power has been held. Simultaneous
Power/selector presses are recognized together, without a navigation press.
When a selector was already navigating before Power was pressed, its ordinary
hold ends when the macro is recognized.

After execution, macro recognition and standalone Power actions stay consumed
until **all physical buttons are released**, including any unrelated buttons
held alongside the macro. This latch survives rejected activity selection and
the one-second `wait_for_release` fallback. A rejected macro consumes its Power
and selector buttons but preserves unrelated holds such as Volume Up. Accepted
activity selection applies the normal held-button quarantine and
`wait_for_release` gate. Disconnect, input loss and activity changes cancel a
pending macro; its remaining held buttons cannot complete another action.

Short Power selects activity 0 from standby or enters standby from an active
activity; long Power repairs the current target. Home, Back, volume, mute,
play/pause and Siri map to their CEC controls (Siri sends Display Information).

Touch swipes use the Gen 2/3 31 mm, 1370-unit pad, a pressure threshold of 20,
7 mm step thresholds, primary-axis locking, and release inertia with a
250 mm/s² deceleration and at most six extra steps. Swipe steps are finite CEC
press/release pairs, coalesced up to ten per direction, and ignored in standby
or while buttons are held. Activity changes, link loss and input overflow cancel
pending navigation and gestures. Atomic's activity shortcuts and arrows remain
unchanged. Both models use the existing Pair/Unpair Remote workflow and share
the single saved peer; unpair the current remote before pairing another.

Received Siri and Atomic button presses/releases are recorded at Info level
with the `remote_input` tag in the device logs (Advanced → Download logs or
`/api/logs`). Entries name the button, such as `Siri: Power pressed`, and include
the Siri button mask or Atomic HID usage. Repeated Siri reports of held buttons
do not add duplicate entries. Logging runs on the input task before gesture and
activity suppression, so a received press is visible even when it takes no
action. Disconnects and input queue overflow reset the logging state.

Host tests exercise the descriptor parser independently of IDF:

```sh
sh tests/ble_test.sh
```

The tests cover Atomic's complete keyboard mapping, 6KRO and NKRO, consumer
arrays, bit offsets, duplicate reports, simultaneous report IDs and maps,
rollover, malformed descriptors, and disconnect releases.
Siri profile, button, gesture and finite-step policies are also sanitizer-tested.
`tests/remote_input_test.cpp` covers overlapping Volume Up/Right holds, keyed
releases, simultaneous aliases, cancellation of all holds, stale reports and
activity quarantine for both profiles. It also checks all five fixed Siri
macros at the 500 ms boundary with either press/release order, Siri-only
recognition, unrelated releases, rejected selection with an independent volume
hold, and the executed-macro latch through all-buttons-up and timeout.
`tests/ble_controller_test.cpp` and `tests/siri_remote_test.cpp` retain adapter
and Power-shortcut coverage, including ordinary input after rejected actions.
Remote Diagnostics and pairing/unpairing UI states can be checked without a
device using `python3 tests/web_browser.py --remote-fixture` in the project-local
Playwright environment.
Radio pairing and bond restoration additionally require a real remote or a BLE HID emulator.
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
  --credentials PRIVATE_JSON --key-tests --reboot
```

The test reads `admin_password` from the private JSON file and records no passwords
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
