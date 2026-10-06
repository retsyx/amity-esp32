# CEC behavior comparison

The golden behavior is the running Python application, not just the mocked
`test/test_hdmi.py` adapters. It combines `main.py`, `hdmi.py`, `cec.py`, and
Linux's CEC core. In particular, `main.py` passes the lowercase OSD name
`amity`; the Python test fixture's uppercase `Amity` is not the application
default. The original port was confined to the old repository’s `esp32/` folder; this repository now contains the standalone project.

For the table below, the front playback address is 4, the back TV address is
0, and the front physical address is the Python default 1.0.0.0. Headers
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
| Ordinary key | Target its source; append Released for a complete press |
| Play/pause toggle | Default Python emulation alternates Play/Pause; repeats keep the same mapped key |

Two deliberate implementation details support the ESP32 architecture:
discovery is asynchronous behind the thread-safe service, and received
device addresses are scoped to their bus. This avoids confusing a front
device with an unrelated device having the same logical/physical address
on the back. The held-key release tracks its actual recipient, including
audio, instead of unconditionally releasing the source.

Matter Speaker support adds Give Audio Status (`71`) and handles Report
Audio Status (`7a`) from the current audio device. Absolute volume/mute are
unknown until reported; volume 127 stays unknown. Explicit mute and restore
keys (`65`/`66`) go to audio. These extra status queries are required for
truthful Matter state and do not change the activity power/routing rules.
Queries after volume/mute input wait for 500 ms of quiet, coalescing repeated
adjustments. TX capacity waits are bounded; failed key releases are retained
for retry, and failed initial presses do not change play/pause emulation.

Validation:

- `sh tests/cec_hub_test.sh` checks wire ACK/arbitration/timing failures,
  startup message order, activity transitions, identity payloads, ignored
  unsupported/badly addressed messages, quirks, and audio feedback under
  ASan and UBSan.
- All four CEC/hub implementation files compile with the project's ESP-IDF
  6.1 ESP32-S3 command lines. Object inspection confirmed ISR code and its
  literal pools are in IRAM and ISR state is in internal RAM.
- `cec_hardware.py` performs exact raw-byte reply checks and concurrent
  requests on both fixture buses. A passing host simulation is not a
  substitute for that hardware result; generated results live under
  `test-results/`.
