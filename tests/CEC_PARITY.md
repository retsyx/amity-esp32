# CEC behavior comparison

The golden behavior is the running Python application, not just the mocked
`test/test_hdmi.py` adapters. It combines `main.py`, `hdmi.py`, `cec.py`, and
Linux's CEC core. In particular, `main.py` passes the lowercase OSD name
`amity`; the Python test fixture's uppercase `Amity` is not the application
default. Button handling follows
[the independent key state in `main.py`](https://github.com/retsyx/amity/blob/main/main.py#L144-L208),
[its per-key transmit coroutine](https://github.com/retsyx/amity/blob/main/main.py#L286-L320),
and [individual asynchronous CEC completions](https://github.com/retsyx/amity/blob/main/cec.py#L669-L683).
The original port was confined to the old repository's `esp32/` folder; this
repository now contains the standalone project.

For the table below, the front playback address is 4, the back TV address is
0, and the front physical address is 1.0.0.0 (the Python default and the ESP32
fallback when no usable back-side address is reported at startup). Headers
change naturally if address allocation chooses another address.

| Trigger | Front response | Back response |
| --- | --- | --- |
| Give OSD Name from front TV / back playback 4 | `40:47:61:6d:69:74:79` | `04:47:61:6d:69:74:79` |
| Get CEC Version | `40:9e:06` | `04:9e:06` |
| Give Physical Address | `4f:84:10:00:04` | `0f:84:00:00:00` |
| Give Device Vendor ID | `4f:87:00:00:00` | `0f:87:00:00:00` |
| Give Features | `4f:a6:06:10:00:00` | `0f:a6:06:80:00:00` |
| Give Device Power Status, no activity | `40:90:01` | `04:90:01` |
| Give Device Power Status, activity source is back 4 | `40:90:00` | `04:90:00` |
| Give Device Power Status from another back source | Front still reports on | Reports standby to that source |
| Abort (`ff`) | `40:00:ff:04` | `04:00:ff:04` |
| Unsupported directed opcode (`fe`), or Feature Abort | No reply | No reply |
| Broadcast identity request, or directed reply request from unregistered 15 | No reply | No reply |

Vendor ID zero is a valid configured value; it is not the kernel's sentinel
for an unavailable vendor ID. Feature arrays are zero-initialized by Python.
Thus neither RC-menu capabilities nor optional deck/audio features are
advertised.

Python selects `INITIATOR | FOLLOWER` mode. With a userspace follower present,
Linux does **not** automatically Feature Abort an unknown directed opcode.
Explicit Abort still receives reason 4 (Refused). Linux also filters incorrect
addressing modes and undersized messages before calling the follower. The
port reproduces these behaviors for every opcode it processes. See
[Linux 6.12 CEC message handling](https://github.com/torvalds/linux/blob/v6.12/drivers/media/cec/core/cec-adap.c#L1905).

Startup announcements are, in order, Report Features, Report Physical
Address, and Device Vendor ID. The allocator tries front addresses
4, 8, 11, 12, 13, and back addresses 0, 14, matching the CEC 2.0 Linux
candidate order. See
[Linux 6.12 address allocation and announcements](https://github.com/torvalds/linux/blob/v6.12/drivers/media/cec/core/cec-adap.c#L1316).

The ESP32 now determines its front physical address on every boot: scan the
back bus, choose its lowest valid non-root reported physical address, or use
the configured fallback when none is available. The back TV announces first;
front allocation and announcements wait for this determination. Later rescans
do not change the result. This topology inference is a heuristic when the
directly attached device stays silent; see the hub component README.

| Activity/device operation | Expected Python behavior reproduced |
| --- | --- |
| Start an activity | Power on newly needed devices; keep shared devices on |
| TV power on | Image View On (`04`) |
| Other device power on | User Control Pressed / Power On (`44:6d`), then Released (`45`) |
| Switch activity | Standby (`36`) only devices no longer used |
| Power off | Apply vendor `power_off` quirk when present, otherwise Standby |
| Power-on quirk | Send configured payload; append Released when it begins with `44` |
| Reselect current activity | Power on its devices again and restore routing |
| Known source | Broadcast Set Stream Path (`86:PAhi:PAlo`) on its bus |
| Missing/absent source with configured switch | Send `44:69:input`, `45`, then front Active Source |
| Already off and power-off requested through `standby()` | Broadcast Standby on both buses |
| Backend Image View On / Active Source while off | Broadcast Standby on both buses |
| Another source tries to take an active activity | Standby that source and restore the selected route |
| Selected source reports a new physical address | Update discovery and restore its route |
| Selected source changes logical address before its OSD reply | Replace the stale name mapping and restore routing once metadata completes |
| A missing activity device is found by rescan | Execute the original power/key operation after the rescan |
| Power query during rediscovery | Retain the previous live device mapping until the new scan is complete |
| Volume up/down/toggle mute | Target the activity's audio device |
| Ordinary key | Target its source; append Released for a complete press unless another held key keeps that recipient's stream open |
| Play/pause toggle | Default Python emulation alternates Play/Pause; repeats keep the same mapped key |
| Hold a key | Submit its first press immediately; only that key's successful completion permits its next press |
| Press another key while the first awaits ACK | Submit the new key independently without waiting for the first key's completion |
| Release one of several held keys | End only that key's hold; already admitted presses finish and the other keys continue |

Each key has independent pressed state and completion bookkeeping, with at
most one outstanding attempt per active hold. There is no repeat timer or
backlog of generated repeats. A pending ACK never blocks another key, a
one-shot command, received-frame handling, activity setup, or discovery.
Normal key-up removes only its own hold and does not advance a global
cancellation counter. Already admitted frames remain in CEC's FIFO and finish,
matching Python's submission/completion behavior. Physical transmission is
still serial on each CEC wire; the two wires progress independently.

The ESP32 scopes received device addresses and held-key recipients to their
bus, avoiding confusion between unrelated devices with the same logical or
physical address. CEC Released (`45`) has no key operand. The ESP32 therefore
releases a receiving `(bus, logical address)` only after every held key there
ends. This extends Python's global all-keys-ended check and source-only release
to clean up the actual recipient, including audio.

For Volume Up down, Right down, Right up, Volume Up remains pressed and
continues repeating. If source and audio are different recipients, Right-up
submits `45` to the source while audio continues. If they share a recipient,
Right-up sends no `45`; Volume Up's later release closes that recipient's
stream. Physical aliases share one logical hold until their last usage
releases. Activity/configuration changes, standby, disconnect and input loss
cancel all held keys and pending input gestures.

The thread-safe service dispatches new commands using zero-wait CEC admission.
Discovery, logical-address claims and activity setup track completions through
short owner-task steps rather than waiting for ACKs. Only commands needing
missing metadata defer until the relevant discovery completes; known-device
operations continue. Command admission is separate from successful CEC queue
admission or transmission.

Outgoing frames live solely in CEC's per-wire FIFO: eight queued-plus-active
slots shared by all frames, including releases. Every
key-press frame gets one wire attempt. A failed admission or TX ends only its
key's hold and makes one final-cleanup decision; unchanged down reports cannot
restart it. A later physical up does not duplicate cleanup. Final release and
ordinary commands use up to three wire attempts. Release admission is immediate:
at eight queued-plus-active messages, a release is dropped and consumed.
Each TX dropped because the FIFO is full is logged with its GPIO and full frame bytes.
No later tick or capacity change retries that admission, including power-quirk
and switch-key cleanup. CEC applies supplied attempt budgets
without interpreting button policy. The
[hub component README](../components/amity_hub/README.md) also documents
per-key and release-all emergency epochs used only when the command queue is
full, plus activity-generation validation.

Matter Speaker support adds Give Audio Status (`71`) and handles Report
Audio Status (`7a`) from the current audio device. Absolute volume/mute are
unknown until reported; volume 127 stays unknown. Explicit mute and restore
keys (`65`/`66`) go to audio. These extra status queries are required for
truthful Matter state and do not change the activity power/routing rules.
Queries after volume/mute input are coalesced with a 500 ms scheduling delay
and no active audio hold. CEC traffic and queue admission can delay the actual
request or reply. A rejected initial press does not change play/pause emulation.

Siri's fixed activity macros follow Python's default Power+Select/Up/Right/Down/
Left combinations, selecting activities 0–4 on a short selector hold and 5–9
when the selector has been held at least 500 ms. They are enabled only for the
Siri profile and are not configurable. The complete mapping is recorded in the
[remote component README](../components/ble_remote/README.md). Either macro
button may be pressed or released first; the selector's press timestamp
determines the duration. An unrelated release does not execute the macro.
Execution consumes the gesture until all physical keys are up, even if activity
selection is rejected or `wait_for_release` expires. Rejected selection consumes
only the macro's buttons and preserves unrelated holds; successful selection
applies activity quarantine and the release gate. Atomic's dedicated activity
buttons retain their separate policy and Power+direction never forms a macro.

Validation:

- `sh tests/ble_test.sh` checks Python's `wait_for_release` gate at startup and
  activity selection: fresh overlapping keys, all physical aliases released,
  final-release/report ordering, one-second fallback without replay, swallowed
  swipes, standby/Power release handling, same-target selection, rejected
  selection, long-Power repair, and no duplicate gate after a local selection.
  Hub tests also check that successful same-target selections notify input
  while repairs and rejected requests do not.

- `sh tests/cec_hub_test.sh` checks wire ACK/arbitration/timing failures,
  startup message order, activity transitions, identity payloads, ignored
  unsupported/badly addressed messages, quirks, and audio feedback under
  ASan and UBSan. Front physical-address tests cover multiple response orders,
  delayed AVR replies, invalid reports, configured fallback, startup ordering,
  and retaining the result until a fresh boot.
- `tests/cec_hub_commit_test.cpp` holds completions pending while admitting
  Volume Up and Right, releases Right independently, and verifies volume
  continues. It also covers per-key failures, consumed final-release cleanup,
  one-shot admission, configuration and activity generation. Activity-settling
  tests verify asynchronous setup completions without blocking command handling.
- `sh tests/cec_bus_test.sh` covers zero-wait admission during an outstanding
  ACK, per-operation terminal results and attempt budgets, FIFO bounds and
  capacity reuse, and independent wire completions under ASan/UBSan.
- `sh tests/ble_test.sh` covers both input profiles, the Volume Up/Right
  overlap sequence, logical aliases, keyed releases, stale reports,
  activity quarantine, disconnect/input-loss cancellation, and all five fixed
  Siri macros. Macro coverage includes both press/release orders, selector-based
  duration at the 500 ms boundary, unrelated releases, rejected selection while
  volume remains held, all-buttons-up consumption after rejection and timeout,
  and exclusion of Atomic Power+direction/Select combinations. Rejected actions
  preserve ordinary input; admitted actions quarantine new presses in the same
  release-first report batch.
- `python3 tests/cec_front_physical_hardware.py --url https://amity-dut.local
  --credentials PRIVATE_CREDENTIALS_JSON --host PI_SSH_HOST` exercises discovery,
  rescan pinning, reevaluation on reboot, and no-responder fallback against the
  isolated two-bus fixture. `--firmware build/amity.bin` installs the image by
  OTA first. The fixture adapters must be front `/dev/cec0`, back `/dev/cec1`
  (or supplied explicitly); the script leaves the synthetic TV/source at
  `1.0.0.0` and reboots the DUT to rediscover it. It preserves activities and
  the configured default. Use `--restore-physical` for another fixture address.
- All four CEC/hub implementation files compile with the project's ESP-IDF
  6.1 ESP32-S3 command lines. Object inspection confirmed ISR code and its
  literal pools are in IRAM and ISR state is in internal RAM.
- `cec_hardware.py` performs exact raw-byte reply checks and concurrent
  requests on both fixture buses. A passing host simulation is not a
  substitute for that hardware result; generated results live under
  `test-results/`.
