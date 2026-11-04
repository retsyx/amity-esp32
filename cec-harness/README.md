# Amity functional test harness

Don't log test run results in this file. This file is only for describing the tests.

A Python functional test declares its DUTs, component topology and assertions.
The common framework configures the run, authenticates DUTs, owns the CEC
adapters and collects evidence. CEC I/O and physical button holds run on the
Raspberry Pi bench, locally or over SSH. There is no boot-time virtual theater,
device profile file or permanently running follower service.

**Functional tests MUST use the existing authenticated HTTP APIs used by the
shipping Amity UI. Never add firmware APIs, test-only commands, authentication
bypasses, or test control surfaces for this framework.** The DUT helpers wrap
`/api/login`, the current activity editor/status/discover/configuration routes,
the existing membership scan/add/remove controls, and operation-status routes.
Additional test helpers must wrap existing UI operations as well. The runner
first provisions the factory-reset main through its shipping Matter BLE setup.
Firmware CEC, activity, input, commissioning and membership behavior is unchanged.

## Layout

- `TEST-HARNESS.md`: bench inventory and wiring; the only GPIO mapping source.
- `amity_harness/`: named DUT control, Markdown wiring parser, topology/address
  assignment, CEC transport and component emulation. Shared `config.py`,
  `testbed.py`, `runner.py` and `discovery.py` own runtime configuration,
  resource/authentication lifetime, CLI/reporting and scan evidence/assertions.
- `amity_harness/enrollment.py`: Pi Matter controller, factory-state checks,
  Wi-Fi enrollment and verification after the main's final reset.
- `setup.py`: Pi software installation, deployment, reboot and readiness checks.
- `setup_matter.py`, `matter-requirements.txt`: installer and pinned dependencies
  for the Pi Matter controller.
- `functional_tests/hdmi_scan.py`: exact HDMI scan checks and the reusable scanned fixture.
- `functional_tests/activity.py`: creates an activity from that scan, starts it,
  repairs the same activity after a power/input disturbance, then verifies
  standby using physical component power commands. It checks that the selected
  source's Active Source announcement causes no Standby, and that an announcement
  during global standby receives broadcast Standby without changing selection.
  Its separate cases cover passive discovery of never-selected equipment during
  standby, switch input, local/member key routing, and client disconnect during a hold.
- `functional_tests/source_theft.py`: adds a second playback device on AVR
  input 2 and verifies that Amity restores the first player after source theft.
  Its isolated-source case places the thief behind another tentacle and
  requires suppression before its announcement reaches the AVR.
- `functional_tests/add_amity.py`: adds the second bench board through the main
  Amity, verifies its membership, then removes and resets that added board.
- `functional_tests/consortium.py`: scans equipment on the main and up to three
  headless members, then checks activity power and source-theft suppression in
  every ownership direction.
- `functional_tests/dual_display.py`: switches between a TV and projector on
  one headless member, checking command completion order and preserving shared
  AVR/player power throughout both activity handoffs.
- `functional_tests/qualified_names.py`: uses identical equipment names and
  addresses on two owners and requires control on only the selected owner.
- `functional_tests/missing_equipment.py`: retains a missing source selection,
  verifies switch fallback and lookup-scan cooldown, then uses its passive
  return without editing the activity.
- `functional_tests/removal_cascade.py`: removes a member with four saved
  activities referencing its source, display, audio and switch equipment,
  rejects a stale confirmation, and verifies exact cascading deletion and
  preservation of unrelated local activities and configuration.
- `functional_tests/default_physical_address.py`: changes adapter defaults on
  the main and a headless member, with and without a scanned back address.
- `functional_tests/hostname.py` (`--test hostname`): changes the main or member
  hostname to its bench label plus `-t`, verifies fresh activity power, routing
  and key commands, and repeats after rebooting the main, member or both.
  Each successful case restores the canonical bench names; failures retain DUT state.
- `functional_tests/ble_keyboard.py`: pairs a Pi BLE HID keyboard and verifies
  taps, overlapping buttons and disconnect cleanup with local/member equipment.
- `unit_tests/`: hardware-free framework checks, separate from functional tests.

The hostname tests also verify saved activities, adapter settings and stable
device identities. The temporary names are `amity-dut-t` and `amity-dut-2-t`;
canonical names are `amity-dut` and `amity-dut-2`. After restart, the test verifies
hostname persistence and reconnection before refreshing physical scan proof.
All hostname and configuration changes use the main's authenticated web UI
operations. USB is used only for secondary chip restarts on the runner computer.
The reset worker needs `pyserial` and `esp-pylib`; use `--usb-python PATH` to
select an ESP-IDF Python environment when the runner Python lacks them. USB
serials come from `TEST-HARNESS.md`. Resets use DTR/RTS control lines and send
no serial bytes, bootloader commands or configuration writes.
Failed hostname tests leave names, activities and membership unchanged for
debugging and stop subsequent scenarios. CEC and HTTP resources are released;
factory reset after debugging restores any board, including the main.

## Pi software setup

Use Raspberry Pi Imager to prepare a Pi 5 with **Raspberry Pi OS Lite 64-bit
(Trixie, Python 3.13)**, network access, a hostname and SSH public-key access.
The installation account must have passwordless `sudo`. From this checkout on
the development computer, run:

```sh
python3 cec-harness/setup.py --host pi@new-harness.local
```

Initial setup records a previously unseen SSH host key; changed keys are rejected.

This deploys the harness, installs its system and Python dependencies, prepares
Bluetooth and Matter, builds the CEC kernel modules with DKMS if needed, and
configures CEC adapters from `TEST-HARNESS.md`. Building a missing driver supports
kernel series 6.12 and 6.18. Setup also grants the harness
account access to CEC, GPIO and the required privileged helpers, and disables
old `amity-cec@` followers. It reboots the Pi when needed, reconnects and checks
readiness. Rerunning the command updates the installation without creating
duplicate adapters. Test scenarios still own their simulated equipment; setup
does not create permanent CEC responders or configure DUTs.

For a checkout already on the Pi:

```sh
sudo python3 cec-harness/setup.py
```

Local installation exits with status `2` if a reboot is required. Reboot, then
rerun the installation command to finish setup and check readiness. Add
`--reboot` to reboot automatically; rerun installation afterward. To inspect an
existing installation without deploying or changing anything:

```sh
python3 cec-harness/setup.py --check                 # on the Pi
python3 cec-harness/setup.py --host pi@new-harness.local --check
```

Setup installs infrastructure only. Supply the private bench Wi-Fi, factory
Matter and initial administration credentials described below when running
tests. The bench LAN must support IPv6 and multicast DNS, and the runner must
be able to reach the DUT's HTTPS administration address.

## Main enrollment

Each invocation of `cec-harness/run.py` requires a **factory-reset
`amity-dut`**. Preparation never resets it implicitly or reuses an enrolled
main. Reachable HTTPS fails preparation. Standard Matter preflight must confirm
zero fabrics and no configured Wi-Fi before the controller supplies the bench
network credentials.

The Pi briefly presses FW RESET to reopen initial Matter setup if its
five-minute boot window expired, then commissions over BLE and verifies the
main's identity through authenticated HTTPS. This runs before firmware upgrades and scenarios.
Secondary boards continue to use **Amity devices → Add**.

The bench label `amity-dut` maps to the factory administration address
`https://amity.local` through `TEST-HARNESS.md`. Its optional Administration
hostname column supplies the default URL; explicit URL options still override it.

A successful run requests the main's reset through the authenticated UI API,
then verifies over Matter BLE that no fabrics or Wi-Fi networks remain. A reset
verification failure fails the run. If preparation or a scenario fails or is
interrupted, the runner skips this final reset and retains the state reached
for debugging; ordinary scenario fixture cleanup still runs. Reset the main
explicitly before the next invocation. If final reset verification fails, inspect
its log before recovery: the reset may already have taken place.

Factory reset clears activities, equipment, Wi-Fi and pairings. It preserves
the installed firmware and factory Matter code, and restores the provisioned
hostname and initial admin password.

Pi software setup installs the Matter controller automatically. To verify just
its pinned files later, run on the Pi from the repository root:

```sh
python3 cec-harness/setup_matter.py --verify-installed
```

The controller uses Linux aarch64, Python 3.13, a powered `hci0` Bluetooth
adapter, and GLib, libnl and libatomic runtime libraries supplied by setup.
Supply credentials for a 2.4 GHz Wi-Fi network reachable from the Pi. The worker
uses a private mount namespace and drops root privileges before loading Matter
to isolate SDK platform storage from other controllers. Enrollment finishes
before the BLE keyboard fixture starts.

The installer verifies or downloads the exact wheels in
`matter-requirements.txt`, installs them under `.matter-python/`, and supplies
official SDK test attestation roots in `paa-root-certs/`. Its wheel cache is
`.matter-wheels/`. These directories are at the repository root.
`--verify-installed` checks the existing installation without changing files.

Store the bench's factory enrollment credentials on the Pi in
`~/.config/amity-harness/enrollment.json`, with mode `0600`:

```json
{
  "wifi_ssid": "bench-network",
  "wifi_password": "replace-with-bench-password",
  "matter_pin_code": 20202021,
  "matter_discriminator": 3840
}
```

Use exactly these four fields, replacing the examples with the bench network
and main's factory values. The numeric PIN differs from the longer printed
manual pairing code. The factory PIN and full discriminator can be read from
the original `amity-factory.bin` provisioning data or decoded from the Matter QR
payload in `amity-pairing.txt`; retain these private provisioning files.
`--enrollment-credentials PATH` optionally supplies this file from the caller.

The separate `--credentials` file lives on the runner host, has mode `0600`, and
contains the main's **provisioned initial** admin password:

```json
{"admin_password": "replace-with-initial-admin-password"}
```

The device must have an initial admin password; automatic preparation does not
perform password onboarding. Factory reset regenerates the HTTPS certificate,
so the bench examples explicitly use `--insecure`. Secrets travel over SSH stdin
and are excluded from reports; the enrollment worker receives no HTTP password
or session token.

After this one-time setup, normal test commands enroll automatically. To check
only enrollment and successful reset cleanup, run from the checkout root:

```sh
python3 cec-harness/run.py --test enrollment \
  --credentials test-results/credentials/amity-dut.json --insecure \
  --harness-host pi@test2.local \
  --output test-results/functional/enrollment.json
```

Omit `--harness-host` when running directly on the Pi. Remote setup installs the
harness at `/home/pi/amity/esp32/cec-harness`; use `--harness-root` for another
installation location.
`--matter-python` selects the Pi's controller interpreter (default `python3`);
it is independent of the BLE keyboard fixture's `--ble-python`.
`--enrollment-timeout` defaults to 240 seconds for main preparation and,
separately, final reset verification, with a bounded worker shutdown allowance.
It is independent of `--timeout`, `--cec-timeout` and `--firmware-timeout`.
`--test enrollment` cannot be combined with `--firmware` and leaves the main
factory reset after success; it does not leave an enrolled interactive session.

After inspecting a failed run, reset through **Advanced → Factory Reset**, hold
the main's FW RESET button for at least ten seconds, or invoke its wired button
from the development computer:

```sh
ssh pi@test2.local 'cd /home/pi/amity/esp32 && ./cec-harness/fw-reset amity-dut 10.5'
```

The button command runs on the Pi and needs no DUT network connection or HTTP
credentials. Subsequent test runs reopen the initial Matter window as needed.

Private controller state lives under
`~/.local/state/amity-harness/matter/<device-id>/` on the Pi. Each enrollment uses
fresh state. `current/commissioning.log` belongs to the latest successful
enrollment; failed attempts retain their `generation-*/commissioning.log`.
Reset checks use `verification-*/commissioning.log`, with `last-verification`
pointing to the latest successful check. Inspect these logs for BLE, credentials,
attestation or network failures; they may contain secrets and must remain private.

## Writing a functional test

The complete scenario is in `functional_tests/hdmi_scan.py`:

`hdmi_scan.py` owns the reusable `scanned_devices()` context. Its setup and
assertions are:

```python
from contextlib import contextmanager
from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent


@contextmanager
def scanned_devices():
    dut = DUT("amity-dut")
    tentacle = dut.tentacle(0)
    tv = TvComponent(tentacle.front, "Scan TV")
    avr = AvrComponent(tentacle.back, "Scan AVR")
    player = PlaybackComponent(avr, "Scan Player", port=1)

    with TestBed(dut, tv, avr, player):
        scan = dut.scan_hdmi()
        scan.assert_contains(tv, avr, player)
        scan.assert_contains_only(tv, avr, player)
        scan.assert_on(tv, tentacle.front)
        scan.assert_on(avr, tentacle.back)
        scan.assert_on(player, tentacle.back)
        yield dut, tv, avr, player


def test_hdmi_scan():
    with scanned_devices():
        pass
```

A functional test contains its equipment and behavior. Wiring parsing,
credentials, CLI options, resource cleanup, evidence helpers and CEC protocol
handling belong in the common framework. The TV, AVR and playback models keep
their existing `handle()` protocol behavior.

`TestBed` accepts DUTs and components in any order, and infers each component's
DUT. It supports several DUTs and configurations through the same interface.
On entry it authenticates the HTTP DUTs, checks their documented identities and
opens their component session. DUTs listed in `control_only` supply documented
physical button control without HTTP authentication or a Wi-Fi/UI prerequisite,
and do not host CEC components. `members=(secondary,)` instead permits a
headless candidate's CEC components in the shared session while keeping HTTP
access on the main alone. Both modes require the named physical DUT to belong
to that same bed. Exit attempts registered UI
resource cleanup, CEC cleanup and logout on success, assertion failure or
exception, in that order.

HTTP requests on each DUT reuse one authenticated HTTPS connection with the
configured TLS verification. The framework fully receives each response before
reusing the socket, expires connections after two idle seconds, and closes them
on logout or testbed cleanup, including failed authentication. A failed request
or redirect is reported without an automatic resend; mutations are never retried
by the transport. Existing per-request deadlines include transport-lock waiting,
connection setup and response reads. Connection events appear at DEBUG level
without credentials, bearer tokens, URLs or HTTP bodies.

`DUT.scan_hdmi()` uses the existing UI scan operations and returns a
`ScanResult`; `assert_contains()` checks the requested component identities and
fresh physical exchanges through the common discovery implementation.
Those exchanges must carry the expected OSD name, physical address and device
type in their actual wire frames; matching cached UI rows alone cannot pass.
`assert_contains_only()` additionally rejects every extra or duplicate scan
entry. `assert_on(component, endpoint)` checks the DUT-reported owner, tentacle
number and front/back side using the existing UI's qualified equipment
observations. It does not infer the reported connection from the fixture's
attachment or parse a display label. The initial scenario requires exactly
these three devices: TV on tentacle 0/front, AVR and player on tentacle 0/back.
For a distributed scan, `member.assert_connected()` must first establish the
member's inventory ID, boot, name and ready settings through the main UI.
`ScanResult.owner_boots` records the main and explicitly proven member boots;
`validate_owners()` checks the current main identity, group and ready membership
against those proofs in the same active testbed and CEC session. Each fixture
must independently provide fresh wire discovery on its own owner's GPIO.
Matching names, logical addresses or coordinator observations cannot substitute
for another owner's wire evidence. Activity boundaries retain these owner boots
and initiators, and component command assertions recheck remote-owner proof.
These checks qualify test evidence; they add no firmware availability policy.

`activity.py` extends this same scan and its assertions:

```python
from amity_harness import PowerState
from amity_harness.transitions import observe_no_standby
from functional_tests.hdmi_scan import scanned_devices


def test_activity():
    with scanned_devices() as (dut, tv, avr, player):
        components = (tv, avr, player)
        with dut.create_activity("Functional activity", display=tv, source=player, audio=avr) as activity:
            for component in components:
                component.assert_power(PowerState.STANDBY)

            started = activity.start()
            for component in components:
                component.wait_for_power(PowerState.ON, since=started)
            avr.wait_for_input(player, since=started)
            activity.assert_active()

            player.standby()
            avr.active_input = 0x1200
            with observe_no_standby(*components) as preserved:
                repaired = activity.start()
                for component in components:
                    component.wait_for_power(PowerState.ON, since=repaired)
                avr.wait_for_input(player, since=repaired)
                activity.assert_active()
                preserved.assert_preserved(since=repaired)

            stopped = activity.standby()
            for component in components:
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
```

`DUT.create_activity()` uses the exact scanned selections and the shipping UI's
authenticated activity editor. It retains existing activities.
Use `add_components=(player,)` to save an unscanned fixture through the UI's
Add component operation before selecting it. A scanned device on the same
owner and wire proves ownership; it does not prove the new component exists.
The activity tests keep that player electrically off through scanning and
creation, then require a fresh physical OSD exchange when its responder returns.
Both the main and an added secondary must subsequently deliver fresh power,
routing, key and standby commands to the manually selected equipment.
Enumeration checks observe that adapter's physical wires; the UI's combined
busy indicator may include other adapters finishing an earlier explicit scan.
The fixture power/input setters above create a disturbance only. Repair must
deliver fresh physical power-on and routing commands; final model state alone
cannot pass. An original-frame observation rejects any Standby, power-off or
toggle command during the repair, including commands hidden by responder hooks.
The separate `test_activity_switch_input()` sub-test configures the AVR as the
switch with `switch_device=avr, switch_input=4` and `source=None`, representing
a non-CEC source on that input. It verifies the selected activity, TV/AVR power
and `avr.wait_for_switch_input(4, since=started)`. That assertion requires a
fresh, directed DUT Select AV Input Function (`44:69:04`) and the AVR's applied
numeric `selected_input`; cached state, virtual traffic or Set Stream Path
cannot satisfy it. The scanned CEC player remains in Standby. A discovered CEC
source uses its physical address instead, preserving Amity's original input
selection behavior.
The activity context attempts to remove its test-owned activity on exit,
including after an assertion fails, before the component responders and HTTP
session close. A DUT restart or conflicting user activity can prevent safe
cleanup; the run fails and reports retained test-owned identities. The client
does not silently authenticate again and continue an action across a reboot.
Shared helpers own revision checks, asynchronous operation completion and
cleanup; functional files contain no HTTP or polling boilerplate.

`assert_power()` checks current state. `wait_for_power(..., since=action)` also
requires a valid physical command received from that DUT after the action began.
Virtual messages, old traffic, dropped/rewritten commands and local power
setters cannot satisfy the command check. Power On must be explicit; merely
being awakened by input routing does not pass. `command="standby"` specifically
requires the CEC Standby opcode rather than another command that changes power.
CEC Standby leaves the component
in `PowerState.STANDBY`. `PowerState.OFF` models disconnected power and has no
CEC traffic. Private reports retain the power commands and assertion evidence.

`source_theft.py` uses `scanned_devices(extra_player=True)`: the same TV and
AVR, plus playback devices on AVR inputs 1 and 2. The second
`PlaybackComponent` explicitly selects `address_type="tuner"` to use a distinct
logical-address allocation profile while retaining playback behavior. The
Linux session uses CEC 1.4 for the three modeled components on the back bus.
The extended scan requires exactly these four components, with the TV on
tentacle 0/front and the AVR and both players on tentacle 0/back. Each player
has a saved activity using the shared TV and AVR. The test starts and verifies
the first player's activity, then calls the second player's `activate()` to
broadcast its own Active Source
announcement. `Component.activate()` returns the transmission's `TxResult`;
the scenario requires a successful physical announcement.

Amity must then send a fresh directed Standby command to the second player and
a fresh Set Stream Path restoring the first player's physical address. Both
must finish within 250 ms of the physical Active Source transmission:
`other.wait_for_standby_after(announcement, within=0.250)` checks Standby and the
resulting state, and
`avr.wait_for_input(player, since=boundary, after=announcement, within=0.250)` checks
route restoration against that same announcement. The 250 ms bound is an
absolute maximum; normal remediation is expected to be much faster.
[Linux CEC timestamps](https://www.kernel.org/doc/html/v6.19/userspace-api/media/cec/cec-ioc-receive.html)
mark the last transmitted/received byte, so the measured interval
includes completion of the response frame. The helpers correlate the trigger
with the exact component, session and wire. Virtual traffic, failed TX results
and a late frame already in history cannot pass. Assertions and stdio logs
retain the trigger timestamp, response latency and maximum interval. A slow
authenticated owner-boot check may finish later, but cannot extend this physical
window or turn a late response into a pass. The first player must be active again;
`activity.assert_active()` verifies that its saved UI activity and DUT boot
remain unchanged. Both test-owned activities are cleaned up before the
responders close. Shared component and activity helpers perform these checks,
so the functional body contains no raw CEC commands or HTTP handling.

`add_amity.py` uses the original main and the second board from the inventory:

```python
from amity_harness import DUT, TestBed


def test_add_amity():
    main = DUT("amity-dut")
    candidate = DUT("amity-dut-2")
    with TestBed(main, candidate, control_only=(candidate,)):
        with main.add_amity(candidate) as member:
            member.assert_connected()
```

The candidate's canonical inventory name is `amity-dut-2`. Only the main uses
authenticated HTTP. The candidate needs harness button wiring;
it does not need its own HTTP credentials, Wi-Fi connection or reachable UI.
The shared helper scans through the existing UI Add flow and holds the
candidate's FW RESET for 3.5 seconds, then releases it. The GPIO assignment is
resolved from the setup/reset table parsed from `TEST-HARNESS.md`, never a
hardcoded bench pin in the framework or tests. With `--harness-host`,
the resolved GPIO is sent to the Pi, which owns the bounded hold and releases
the pin even if the requesting process disconnects. The normal admission
profile uses no setup code.

`member.assert_connected()` verifies the candidate's full inventory device ID,
assigned name, ready membership, configuration entry and device settings. It
also checks that the original main, its authority and its saved activities
remain intact. Once the peer and saved configuration are ready, the helper
polls the existing device-settings endpoint directly so unrelated TLS requests
do not outlive its two-second response cache. After a matching settings reply,
it rechecks the main boot/authority, existing saved activity/equipment/settings
records, membership, member boot and name before passing. This uses the same APIs and overall operation
deadline; it changes neither the firmware cache lifetime nor assertion criteria.
On exit, the helper removes only the new member that this scope
successfully identified, using the existing Remove action with reviewed
configuration/presentation and membership revisions, target boot and the exact
affected activity list. Cleanup requires acknowledgment that the candidate's
reset was scheduled and verifies its saved owner, equipment, adapter settings
and named inventory are gone; this deliberately resets the added board. A
candidate referenced by preexisting user activities is rejected before Add,
so the test cannot authorize deleting those activities.
The Add scenario itself does not factory-reset the main, run a ten-second reset before the test,
or remove a preexisting member. Failures retain/report uncertain ownership
rather than removing another device.

Opening the diagnostic serial ports also caused unintended
`USB_UART_CHIP_RESET` reboots on both boards; serial capture was stopped.
These reboots were separate from the candidate's `fw_reset` stack overflow.
The main's saved configuration and controller identity were preserved, and its
existing UI reports two current Matter fabrics after the diagnostic reboot.
The main ran 0.1.88 in those initial attempts. An admission-only pass with
those mixed versions would not qualify distributed power commands, whose
message format changed between those versions.

Subsequent debugging exercised the candidate's setup-stack fix on 0.1.89,
including a 3.5-second gesture with 3,968 bytes of minimum free task stack.
Further failures exposed missing ATT MTU negotiation on the main, rollback
before candidate DHCP completed, settings polling that missed the existing
two-second response cache, and an in-flight settings reply ahead of the removal
acknowledgment. The current 0.1.92 source addresses those paths without new
firmware APIs or extended transaction deadlines. All 240 framework unit tests
pass within the completed full host runner, and the final 0.1.92 native build
passes all 35 public-record layout checks. The original test on main 0.1.92
and candidate 0.1.91 completes Add and the exact connected-member/settings
assertions, but still fails cleanup: Remove reports reset `unconfirmed`, and
the candidate logs flash-cache stack-safety and later semaphore assertions.
Its unclaimed recovery and removal/reset acknowledgment are not qualified.
Earlier failed runs remain failures. Current private debug evidence is under
`test-results/add-amity-debug-2026-10-29/`.

Further candidate diagnosis traced those assertions to a restart shutdown hook
writing NVS from a PSRAM stack and administration mDNS removal on a headless
board with no registered responder. Candidate 0.1.93 uses the existing internal
flash-stack broker for the same restart and guards the owned mDNS service's
lifetime. Main 0.1.94 also keeps a completed Add operation complete after later
removal; the completion is recorded only after exact ready-member/name proof,
and connected-member/settings checks still require current evidence. Native
builds and the final verified full host runner pass, including all 240 framework
unit tests. Both consecutive unchanged physical tests on main 0.1.94/candidate
0.1.93 pass Add, connected/settings checks and cleanup with reset `scheduled`,
no retained member and no cleanup errors. The candidate returns to unclaimed
Ready and automatic Matter setup after both resets without panic; minimum free
setup-task stack is 3,968/3,976 bytes. Final HTTP checks preserve the same main
boot/authority, exact configuration and two Matter fabrics. Main USB is untouched.
Evidence is `add-amity-main-0.1.94.json`, `add-amity-repeat-main94-candidate93.json`,
`add-amity-main94-instrumented.json` and `add-amity-main-0.1.94-after.json` in the
debug directory. This qualifies this pair's normal-profile Add/removal path,
not CEC/AV or the strict setup-code profile. Boards 3/4 remain on 0.1.88, and
the earlier failed runs are preserved.

`consortium.py` uses `DUT("amity-dut")` as the original main and
`DUT("amity-dut-2")` as a headless member. Each has one TV on tentacle 0/front,
one AVR on tentacle 0/back, and one playback device on AVR input 1. The six
components share one `TestBed` with `members=(secondary,)`, using four isolated
CEC wires. The main's existing Add flow enrolls the second board before a
coordinator scan must find exactly all six identities on their actual owners,
tentacles and CEC sides. Only the main uses HTTP credentials.

The scenario saves one activity for each TV/AVR/player triple. It first starts
the main's activity, then the member's, with a standby cycle between them.
Each cycle starts with all six components in Standby and requires fresh
physical power-on commands for the selected three and correct initial AVR
routing. The other owner's player then sends its own physical Active Source
announcement. That owner must send a fresh physical directed CEC Standby to
the thief because it differs from the selected source. The directed Standby
and resulting state must satisfy the same Active Source-to-response 250 ms
wire-completion limit as the shared-wire test; its TV and AVR must
remain in Standby. The active three must remain On with their selected
input and player active, and the main UI must retain the same activity and boot.
The current activity is the sole source of truth, with the special standby
activity when the whole system is in standby. The activity's qualified source
determines the owner-local mismatch; global standby permits no active source.
There is no separate per-owner inactive state or flag.
The wires are isolated, so this theft does not require a new Set Stream Path
on the unaffected active owner. This differs from the shared-wire
`source_theft.py` scenario, which requires physical route restoration.

Returning each selected activity to standby requires fresh CEC Standby on its
three components and leaves all six in Standby. Both temporary activities and
their unreferenced fixture equipment are cleaned up before the member is
removed with a scheduled reset, while component responders remain available.
Normal reserved activity-slot tombstones are retained; cleanup accepts only
new default-metadata tombstones tied to successfully created and removed
fixture activity IDs, preserving existing slots and user metadata.

`test_consortium_three_duts()` adds `amity-dut-2` and `amity-dut-3` to the
original main `amity-dut` in the same bed, with both candidates listed in
`members`. Each owner has the same TV/front and AVR plus input-1 player/back
topology. It requires exactly nine scanned identities, nine typed locations
and fresh discovery on all six isolated wires. Only the main uses HTTP.

The three-DUT case creates one activity per owner, starts each in turn, and
tests theft from both other owners while it is selected: six directed theft
checks in total. Each cycle requires fresh power-on and standby commands for
its active triple, retains its AVR route and selected activity during both
theft attempts, and leaves all nine components in Standby. All three activities
and their fixture equipment are removed before member 3 is removed/reset,
followed by member 2, with responders still running. Member 2 is verified before
the second Add and again after member 3 is removed; the distributed scan and
activity assertions retain and recheck both members' proven boots throughout.
Select it with `--test consortium_three_duts`. Host checks verify its coverage
and cleanup order; adding the case does not physically qualify three-owner
operation.

`test_consortium_four_duts()` extends the same test to `amity-dut` as the main
and `amity-dut-2`, `amity-dut-3` and `amity-dut-4` as headless members. Each
owner uses tentacle 0 with the same TV/AVR/player topology. The scan requires
exactly twelve identities and typed locations across eight isolated wires.
The four activities each run a power-on/standby cycle and receive a physical
theft attempt from every other owner, for twelve directed theft checks with
the same 250 ms deadline. Activities and fixture equipment are cleaned up
before members are removed/reset in reverse Add order, with responders still
running. Select it with `--test consortium_four_duts`; it also runs in
`--test all`.

## DUTs and wiring

`DUT("amity-dut")` implicitly reads `TEST-HARNESS.md`.
`Wiring.from_markdown()` finds the **HDMI-CEC harness GPIO mapping** table and
builds `(DUT name, tentacle, front/back) -> Pi GPIO`. It also reads DUT identities
and the setup/reset GPIO table. Duplicate assignments, unknown DUTs, missing
paired sides and overlapping pins are rejected before hardware access.

The runner supplies a scoped `RunConfig` for credential files, DUT URLs, TLS,
wiring, timeouts and CEC transport. The credential JSON contains `admin_password`
and must be private (mode `0600`). The default HTTP origin uses the inventory's
Administration hostname, falling back to the bench label when that field is
blank. Thus `DUT("amity-dut")` uses `https://amity.local`. Use `--url` or per-DUT
`--dut-url DUT=URL` to override it. TLS verification is enabled by default:
supply `--cafile` for a trusted bench CA or explicitly choose `--insecure` for
its self-signed certificate.
Use `--credentials PATH` for the run's default credentials, or repeat
`--dut-credentials DUT=PATH` for individual DUTs. `--wiring PATH` selects a
wiring document. These settings stay outside the functional test body.
Passwords and session tokens are never included in test reports.

Call `dut.press_fw_reset(3.5)` to request a physical 3.5-second hold; the
firmware interprets the gesture. FW RESET is active low. A hold drives only
its documented Pi GPIO low and releases the pin to input with pulls disabled
in `finally`, including on
interruption. A three-to-below-ten-second hold has the existing Add gesture
meaning; ten seconds has the existing factory-reset meaning. The runner enrolls
the already factory-reset main before the HDMI scan scenario. The scenario
itself does not rename the DUT or change saved activities.

For a standalone button hold on the Pi, run from the checkout root:

```sh
./cec-harness/fw-reset amity-dut-2 3.5
```

Add the harness directory to your shell's `PATH` to use `fw-reset` directly:

```sh
export PATH="$PWD/cec-harness:$PATH"
fw-reset amity-dut-2 3.5
```

The command uses `DUT.press_fw_reset()` and the same wiring lookup and GPIO
release handling as the functional tests. It needs `pinctrl` but no DUT HTTP
credentials. It retains the framework's duration limits: positive, finite holds
up to 60 seconds, excluding holds longer than 8 and shorter than 10 seconds.
An explicit hold of 10 seconds or more retains the factory-reset gesture.

## Components and topology

Each `Component` attaches to a tentacle endpoint or another component's HDMI
input. `dut.tentacle(0).front` connects the TV; `.back` connects the AVR. A
playback component attached to the AVR inherits its DUT, tentacle, CEC side and
GPIO. The attachment determines those values, so a child cannot independently
select a different wire. Each component has a logical and physical `CecAddress`,
a name, a device type, and `PowerState.ON`,
`PowerState.STANDBY` or `PowerState.OFF`. Standby remains CEC responsive; Off
removes the logical address and suppresses CEC TX/RX, including ACKs. Power can
be changed explicitly with `set_power()`.

`TvComponent`, `AvrComponent` and `PlaybackComponent` provide role-specific
identity, discovery, power and control handling. They are configurable test
models, not implementations of every feature in the HDMI-CEC specification.
Components can transmit frames, process received frames, and use before/after
TX/RX hooks to observe, change or drop component messages for fault injection.
Hooks run synchronously during message delivery, including in the session
worker; keep them short and nonblocking. A hook or RX handler may transmit a
nested reply. Delivery is limited to 32 nested levels to reject echo loops,
without adding a message queue or delay. Driver-level ACKs and logical-address
claim traffic are outside these message hooks.

```python
from amity_harness import (
    DUT, AvrComponent, CecSession, PlaybackComponent, TvComponent, assign_addresses,
)

dut = DUT("amity-dut")
tentacle = dut.tentacle(0, tv_input=1)
tv = TvComponent(tentacle.front, "Scan TV")
avr = AvrComponent(tentacle.back, "Scan AVR")
player = PlaybackComponent(avr, "Scan Player", port=1)
components = [tv, avr, player]
assign_addresses(components)
# TV: 0 / 0.0.0.0; AVR: 5 / 1.0.0.0; player: 4 / 1.1.0.0.
with CecSession(components) as session:
    # Component RX/TX handling runs while DUT HTTP operations execute.
    session.assert_healthy()
    result = player.send(avr.address.logical, 0x44, [0x41])  # Volume Up
    assert result.transport == "virtual"  # No GPIO TX for this exchange.
    player.send(avr.address.logical, 0x45)  # Release
```

The HDMI attachment chain is:

```text
TV input 1 -- front [Amity tentacle 0] back -- AVR input 1 -- Playback
```

The tentacle is inline and adds no HDMI physical-address level. `tv_input=1`
selects the TV input containing this chain: the TV is `0.0.0.0`, the directly
attached AVR is `1.0.0.0`, and its input-1 player is `1.1.0.0`. Choosing
`tv_input=3` instead gives the AVR `3.0.0.0` and player `3.1.0.0`.
The TV and AVR attach to opposite DUT endpoints; the AVR has no direct TV
component parent. Component children inherit their parent's isolated CEC bus.
The inline HDMI path keeps the two front/back CEC wires isolated.

Address assignment follows these attachments and input ports for physical
addresses and component type for logical addresses. Logical addresses are
independent on each isolated CEC wire. Explicit addresses are accepted and
validated; a standalone back component can supply its base physical address
when a front TV is outside the modeled fixture. Optional
`reserved={gpio: {logical_addresses}}` avoids known existing occupants. Cycles,
duplicate addresses, invalid parent ports and excessive depth fail before I/O.

A `CecSession` treats each isolated GPIO as a separate virtual CEC bus.
Components on the same GPIO can exchange directed messages and replies without
sending those messages through the Linux adapter. A powered target returns a
successful virtual ACK (`status=1`); an Off target returns a virtual NACK
(`status=4`) and receives no frame or RX hooks. A component does not ACK its own
directed messages or polls. Polls report powered presence without calling an
RX handler. Virtual TX and RX use the same monitoring/fault hooks as other
component messages.

Broadcasts reach the powered sibling components on that virtual bus and are
also transmitted once on its physical wire for the DUT. Virtual delivery and
wire transmission are independent: a wire failure does not undo the virtual
delivery. The sender receives the actual wire result, and its `after_tx` hook
runs once. Different GPIOs remain isolated, including front/back sides,
adapters and DUTs; topology parent links assign physical addresses and do not
bridge their CEC buses.

TX/RX records label their transport as `virtual` or `wire`. Virtual records have
`wire_timestamp_ns=0` and never count as evidence of DUT wire traffic. The
session's virtual ACK represents component-model delivery, not an electrical
ACK from the Linux adapter. RX records preserve the original transport frame
and timestamp when a fault hook changes handling; `handled_frame` and
`handled_timestamp_ns` describe the modified frame separately.

The Linux backend finds `cec-gpio` adapters by stable `cec@<GPIO>` identity,
never `/dev/cecN` numbering. Components on a wire share one adapter and a worker.
It checks logical-address capacity, claims exactly the requested addresses,
uses passthrough mode for replies to the DUT and clears dynamic addresses when
the session closes. Linux chooses logical addresses by allocation type; unsupported
claims or collisions fail explicitly. Linux's restriction on electrical TX
between addresses claimed by one adapter applies only to the physical backend;
the session routes component-to-component messages on the virtual bus. No fixed
follower processes are needed. The bench's GPIO adapters support four logical
addresses. Compatible fixtures keep CEC 2.0; larger or differently paired
fixtures explicitly use CEC 1.4. Components advertise that selected version and
omit CEC 2.0 feature reports in 1.4 mode. Remote workers report the same version;
update both ends together when changing the harness RPC protocol.

`PlaybackComponent(..., address_type="tuner")` selects the tuner address pool
while retaining playback identity and behavior. The default is `"playback"`;
`"recording"` is also available for compatible topologies. Linux rejects
duplicate allocation types and the recording-plus-playback combination even
in CEC 1.4. The source-theft test therefore uses AVR address 5, the selected
player at playback address 4, and the second player at tuner address 3 on the
same back wire, with distinct physical addresses `1.1.0.0` and `1.2.0.0`.
Claim failures remain explicit; the backend does not retry using another type.

## Dual-display activity scenario

`functional_tests/dual_display.py` uses `amity-dut` as the original main and
adds `amity-dut-4` as a headless member through the existing Add flow. Only the
main needs HTTP credentials. The TV connects to member tentacle 1/front; the
AVR's CEC-capable output connects to 1/back, with the player on AVR input 1.
The projector is a CEC display on 2/front. The AVR's second output connects to
2/back without CEC support, so that wire has no component responder. The AVR
and player appear once, on 1/back; the two back wires are never bridged.

The scan requires exactly those four identities and their typed owner,
tentacle and side locations. Two saved activities share the AVR and player
while selecting either the TV or projector. Starts from Standby require fresh
physical power-on commands and AVR routing. The first cycle starts the TV
activity, changes directly to the projector activity, then enters Standby.
The second starts the projector activity, changes directly to the TV activity,
then enters Standby. Each direct change requires a fresh power-on command to
the selected display and fresh CEC Standby to the previous display, while the
AVR and player remain On.
The handoff observation starts before activity submission, rejects any physical
power command to the shared AVR/player, and requires the previous display's
Standby completion before the selected display's power-on completion, followed
by the AVR's Set Stream Path completion. It drains every involved receiver
through the assertion boundary so queued evidence or a transient Standby
cannot hide behind a later On state. Original wire frames remain visible even
if component hooks drop or rewrite their handling. Cross-wire comparisons use
conservative SSH clock bounds. Linux timestamps describe complete frames;
these checks establish completion order, not first-edge timing or proof that
frames on isolated wires never overlap.
Every selected activity and its route are verified. Here, an unused display
being "off" means CEC Standby; electrically unpowered `PowerState.OFF` would
remove its CEC responder.

Both activities and their fixture equipment are cleaned up before the newly
added member is removed with acknowledged scheduled reset, while responders
remain available. Select `--test dual_display`; it also runs in `--test all`.

## Source theft before the AVR

The isolated-source case in `functional_tests/source_theft.py` adds
`amity-dut-4` to the original main `amity-dut`. Its TV attaches to tentacle
1/front, the AVR output to 1/back,
and the ordinary media player to AVR input 1. AVR input 2 attaches to tentacle
2/front, with the thief on 2/back. `AvrAttachment` represents the same AVR at
the second endpoint: power, audio and input state are shared, while received
frames and physical evidence stay separate. It does not forward frames between
harness GPIOs. The scan requires five qualified observations for the four
physical devices, including the AVR on both connected endpoints.

Without EDID, `member.front_physical_address(2, "1.2.0.0")` uses the existing
authenticated UI Save operation to set the default for AVR input 2 without
reboot. It reviews the member boot, settings revision, configured adapter and
latest stored scanned address, then proves the same boot, exact saved defaults
and correct selected address before scanning. Saving selects that scanned
address when present, otherwise the new default. Ordinary scans can update or
clear the observation without changing selection until the next default edit.
After activities close the helper restores the prior saved default through the
same API and verifies selection against the latest stored scan result on the
unchanged boot before member removal. Settings preserve scan proofs and device
caches and do not themselves enumerate.

The media-player activity requires fresh power and AVR routing commands. Three
physical Active Source attempts from the isolated thief must each receive
directed Standby within 250 ms of that exact transmission. An observation
started before each attempt checks both AVR-side wires for leaked source or
routing announcements throughout the independent CEC timeout. It checks original
physical RX before component hooks, so later route recovery cannot hide a leak.
The media player, route and selected activity must remain unchanged. The other
activity must power the thief and select AVR input 2 through a fresh physical
Active Source on tentacle 2/front. This positive control prevents a disconnected
observation path from passing the suppression test. Both activities must also
return their selected devices to Standby through fresh commands.

Select `--test source_theft_isolated`; it also runs in `--test all`. Functional
scenarios contain equipment and behavior only; the shared framework owns the
settings, same-boot address proof, protocol monitoring and cleanup.

Cleanup finishes activity rows and exact, unreferenced fixture equipment before
restoring settings and removing members. An unused activity can defer behind
another unchanged activity from the same bed; its shared equipment is revisited
after those references disappear. Edited records and user activity references
remain protected. Activities with retained equipment are revisited after sibling
cleanup retries, before settings and member removal. Explicit HTTP 409 removal rejections receive fresh reviews
and request IDs; unknown outcomes are reconciled with their original operation
ID. A verified inventory deletion permits independent fixture cleanup to
continue, while a missing reset acknowledgment still fails the original cleanup.

Failed CEC transmissions now record the GPIO, complete frame, terminal result,
actual attempt count and whether transmission began. These diagnostics preserve
the original attempt budgets and distinguish wire failures from cancellation
or expiry; they do not retry failed work.

The earlier three-owner deadline failure had no directed Standby, including a
late one, in the remaining wire capture. The retained firmware log did not cover
that event, so it cannot establish whether the announcement was received or a
response transmission failed. Preserve the failed report; a later passing run
does not explain that original failure or relax the 250 ms deadline.

## Qualified equipment and missing-source recovery

`functional_tests/qualified_names.py` gives TV/AVR/player triples on `amity-dut`
and a newly added `amity-dut-2` identical OSD names, logical addresses and
physical addresses. The scan must still report six exact owner/adapter/side
identities. Each owner's activity must deliver fresh physical power, routing,
volume and navigation commands to that owner's equipment. Original-frame
observations reject unintended power, routing or key commands on the other
owner's wires, including transient commands hidden by hooks. Both directions
finish in Standby, and fixture configuration closes before member removal.
Select `--test qualified_names`.

`functional_tests/missing_equipment.py` saves a local activity with a CEC source
and explicit AVR input fallback. It electrically removes the source responder,
then explicitly scans to establish its absence. Starts of the unchanged saved
activity must retain its source selector and send fresh numeric switch-input
commands. Repeated misses during the real 120-second cooldown must not scan;
expiry alone must not launch deferred enumeration. A subsequent real miss
must scan only the source's back side, preserve the front cache, and complete
that original operation's fallback.

Restoring the responder must yield a fresh physical announcement and targeted
identity exchange plus the correct UI observation without another full scan.
The unchanged activity must then power and route the returned source and send
a fresh key press/release to it. Select `--test missing_equipment`; this case
deliberately waits for the real cooldown and therefore adds over two minutes
to `--test all`. The operation and ordinary CEC assertion budgets remain
independent of that elapsed wait.

Reclaiming a Linux CEC address can reannounce other active peers on the same
wire. Their fresh, directed identity queries are passive discovery, not bus
enumeration. The lookup observer correlates each such query to that exact
peer's successful original physical PA announcement on the same wire, within
the CEC evidence budget. It still requires the original boot, unchanged scan
generation and no scan in progress; unannounced witness queries fail. Reports
retain both accepted correlations and rejected query evidence.

## Removal with referenced equipment

`functional_tests/removal_cascade.py` scans TV/AVR/player triples on the main
and a newly added member. Four saved activities independently select member
equipment as source, display, audio or switch, while a local activity keeps
only main equipment. The test reviews Remove, creates another unrelated local
activity, and requires the old confirmation to be rejected without deleting
configuration or changing membership. A fresh review must identify the exact
four affected activities before removal is confirmed.

Confirmation must delete those activities, their ordering references, the
member's equipment, owner record, name and adapter settings, and acknowledge
its scheduled reset. Unrelated records and slot metadata remain unchanged;
affected activity slots retain only ordinary default tombstones. The surviving
local activity then proves fresh physical power, routing and standby commands.
Dependent activity contexts close only after the cascade has been verified,
so ordinary fixture cleanup cannot conceal a missing firmware cascade. Select
`--test removal_cascade` for this case.

## Default physical address changes

`--test default_physical_address` runs four cases on adapter 1 of `amity-dut`
and an added `amity-dut-2`. A front TV alone establishes no scannable back
address; adding a back AVR and its player establishes the detected address
`1.0.0.0`. After an explicit scan, each case changes the default from
`1.0.0.0` to `2.0.0.0` and back. The selected address must follow the default
when no address was detected, and retain the scanned address even when it
equals the old default. Each save checks the three UI address values, unchanged
owner boots and fresh physical Report Physical Address replies. A changed
selection also requires the automatic identity announcement before the test's
query. Discovery checks cover the tested adapter's wires; the UI's combined
scan indicator also includes other adapters. Cleanup restores the original
default before removing the added member.

## BLE keyboard input

`--test ble_keyboard` runs two cases with equipment on the main and on an
added `amity-dut-2`. The Pi advertises a real BLE HID keyboard; pairing and
unpairing use the shipping authenticated remote settings API. The main must
have no saved remote. The fixture refuses to replace an existing bond and
removes only its own new bond during cleanup.

Each case requires a fresh Select press/release, holds Volume Up while pressing
and releasing Right, then disconnects BLE during another held Volume Up.
Original physical CEC frames must prove that Right releases independently,
Volume Up continues, and disconnect ends the remaining stream. No `/api/key`
request supplies these inputs. Generic HID exercises the keyboard input path;
Siri touch gestures and Power macros remain separate.

Pi software setup installs BlueZ and the distribution's `python3-dbus-next`;
the normal `python3` works without `--ble-python`. A custom isolated environment
is optional:

```sh
python3 -m venv /home/pi/amity/ble-keyboard-venv
/home/pi/amity/ble-keyboard-venv/bin/python -m pip install dbus-next==0.2.3
```

For that environment, use `--ble-python /home/pi/amity/ble-keyboard-venv/bin/python`
together with the existing `--harness-host` and `--harness-root` options. Only BLE fixture
control and HID reports cross SSH; DUT credentials remain on the runner host.
The worker releases buttons, unregisters its services and advertisement, restores
the Pi's pairing setting and removes its new peer bond on exit. It does not
restart Bluetooth or remove preexisting Pi bonds.

## Firmware upgrade prelude

Add `--firmware build/amity-update.bin` to a functional run to upgrade all boards
listed in `TEST-HARNESS.md` before the selected tests. For example:

```sh
python3 cec-harness/run.py --test all --firmware build/amity-update.bin \
  --credentials test-results/credentials/amity-dut.json --insecure \
  --harness-host pi@test2.local \
  --output test-results/functional/with-upgrade.json
```

Use `--test firmware_upgrade --firmware PATH` to run only this scenario. Without
`--firmware`, ordinary runs do not upload or run the upgrade scenario. A failed
upgrade stops the remaining tests. `--firmware-timeout` defaults to 2700 seconds
and does not extend physical CEC assertion deadlines.

Start with a factory-reset main and unclaimed secondary boards. The shared
preparation enrolls the main before this scenario. The file must differ from
the running image.
The scenario adds every secondary, exercises an activity per owner, uploads once
through the main, checks staging, new boots, exact image identities and retained
configuration/settings, then exercises the same activities again. Cleanup
deletes its activities/equipment, removes every secondary and requires each
factory-reset acknowledgement, and verifies the main's original settings and
empty membership. The reset acknowledgement proves durable scheduling; erased
secondary Wi-Fi credentials prevent observing reset completion through HTTP.

## Run functional tests

`activity.py` exercises remote key routing twice: once with the TV/AVR/player
on `amity-dut`, and once on the headless `amity-dut-2`, enrolled and controlled
through `amity-dut`. Volume Up, Volume Down and Mute must reach the AVR;
the four navigation directions, Select, Play, Pause and Stop must reach the
player. The combined Play/Pause button must emit Play then Pause after Stop.
Each assertion requires fresh physical directed User Control Pressed and a
later operand-free User Control Released from the scan-proven owner initiator.
It rejects virtual/stale/modified evidence and the same key sent to another
fixture recipient, and rechecks the main's activity/boot and exact member boot.
The Power button must select system Standby and produce fresh physical Standby
commands at all three selected devices. These key cases close their fixture
activities and equipment before member removal/reset.

`DUT.press_key(Key.SELECT)` wraps the shipping authenticated `POST /api/key`.
Its `duration_ms` defaults to zero for a complete tap and accepts at most 5000
milliseconds; firmware owns the final release even if the client disconnects.
The routing cases include zero-duration taps and a 100 ms Volume Up hold.
Key reports retain the requested duration, actual press/release wire timestamps,
recipient and owner boots. No separate release request is needed.

Two additional `activity` cases run a bounded Volume Up hold locally and on the
member, prove its first physical press, and close that authenticated HTTP
connection while the hold is still pending. They require repetition after the
disconnect, a final operand-free release by the original hold/CEC deadline,
and no later presses throughout an additional CEC observation window. The
release must not complete before the requested duration measured from request
start; an overlapping wire-clock uncertainty interval cannot prove that bound.
A fresh Select press must work afterward. The shared helper owns the cancellable
request and receive-drain evidence; the scenario contains no socket handling.

The runner requires Python 3.10 or newer on the development computer. Pi-side
dependencies, CEC adapters and permissions are installed by
[Pi software setup](#pi-software-setup). The runner and CEC helpers use the
standard library; enrollment uses the Pi's pinned Python 3.13 Matter controller.

From the checkout root **on the Pi**:

```sh
python3 cec-harness/run.py \
  --credentials /private/path/amity-dut.json --insecure \
  --output test-results/functional/hdmi-scan-framework.json
```

The default remains `hdmi_scan`. Select `--test activity` for the activity
power-cycle/repair, passive standby discovery, explicit switch-input, two manual-component,
two remote-key and two disconnected
hold sub-tests,
`--test source_theft` for unselected-source recovery,
`--test add_amity` for adding and removing the second board, `--test consortium`
for the two-owner equipment scenario, `--test consortium_three_duts` for the
three-owner scenario, `--test consortium_four_duts` for the four-owner scenario,
`--test dual_display` for the TV/projector scenario on `amity-dut-4`,
`--test source_theft_isolated` for suppression ahead of the AVR,
`--test qualified_names` for identical equipment identities on two owners,
`--test missing_equipment` for source disappearance, cooldown and passive return,
`--test removal_cascade` for stale removal confirmation and exact activity/member cleanup,
`--test removal_replacement` for removing an earlier member, retaining a later member's
saved identities, then adding a different board and proving both owners' physical control,
`--test default_physical_address` for four local/member scanned/fallback cases,
`--test hostname` for local and two-board hostname changes across reboots,
`--test ble_keyboard` for two real BLE keyboard cases (requires an unpaired main),
or `--test all` for all sixteen registered scenarios (twenty-nine
test cases). The same credentials, TLS, SSH and output
options apply. Reports with multiple entries have `passed` and `tests`; each
entry keeps the ordinary single-test report format. Even a successful
single-scenario CLI run includes `prepare_main`, its scenario entries and
`reset_main`. A preparation failure produces only its failed entry and prevents
firmware upgrades and scenarios from running. Any scenario failure skips the
final reset. Successful enrollment-only runs contain preparation and reset
verification entries.

`--timeout` defaults to 180 seconds for HDMI scans, membership and activity
operations.
`--cec-timeout` independently defaults to five seconds for ordinary fresh power
and routing assertions. Main preparation/reset use the separate 240-second
`--enrollment-timeout`. Source-theft remediation has the stricter 250 ms physical
window described above,
including both Standby and shared-wire route restoration, regardless of either
option. Owner identity proof still uses bounded HTTP requests and can extend
the caller's wall time while a request is in progress; accepted physical
response timestamps must remain inside their original window.
The shared framework logs progress to stderr as it runs, with a UTC timestamp,
elapsed run time, run ID and test name on each line. It records setup, identity
checks, scans, activity and membership operations, component assertions,
failures and cleanup. PASS/FAIL summaries go to stdout/stderr after each test.
After writing the JSON report, the final `run.report` line gives its absolute
path on successful and failed runs. Interruptions can exit before JSON report
writing; retain the redirected trace when debugging an interrupted run.

The Pi's status LED blinks red while the functional runner is active, including
test cleanup and report writing. Its previous LED triggers and brightness are
restored when the run ends or its connection closes. Overlapping runs keep the
indicator active until the last run finishes. This also works with
`--harness-host`; LED writes use non-interactive `sudo` when needed. Unsupported
hosts or unavailable LED permissions produce a diagnostic without changing test
results. Use `--no-status-led` to leave the LEDs unchanged.

Owner-proof failures retain the rejected check's bounded identity, boot,
readiness and proof fields in stdio and `scans[].owner_failures` in the JSON
report. Primary failure and cause stacks survive cleanup failures. Only
framework-owned assertion messages are included; arbitrary exception text,
HTTP bodies, credentials and opaque tokens are excluded.

Use `--log-level DEBUG` to include HTTP method/route, request duration and
status, and individual CEC frames with owner, GPIO and wire/virtual transport.
HTTP bodies, headers, credentials and opaque tokens are excluded. The runner
does not create a separate progress log; native Matter logs remain on the Pi as
described above. Capture standard output and error to retain the runner trace:

```sh
python3 cec-harness/run.py --test all \
  --credentials /private/path/amity-dut.json --insecure \
  --log-level DEBUG --output test-results/functional/all.json \
  > test-results/functional/all.log 2>&1
```

Create the redirect's parent directory before running that command. Log events
are flushed as they are emitted, so a redirected trace retains completed steps
even if the process is interrupted. Existing JSON evidence remains separate.
The Add Amity test uses only the main's credentials in the normal admission
profile, for example:

```sh
python3 cec-harness/run.py --test add_amity \
  --credentials /private/path/amity-dut.json --insecure \
  --harness-host pi@test2.local \
  --output test-results/functional/add-amity.json
```

The HDMI scan test uses only `amity-dut`, one TV on tentacle 0/front, one AVR on
tentacle 0/back, and a playback device on AVR input 1. The player shares the
AVR's back CEC bus through that attachment. It checks the DUT identity, submits the UI's
HDMI scan, and requires both fresh discovery exchanges on the CEC wires and the
correct names, types, logical addresses and physical addresses in the completed
UI scan. It rejects all additional entries and verifies each component's
reported tentacle and side from typed UI identity data. Cached names or a scan
submission acknowledgment alone cannot pass.
Each component must answer fresh Give Physical Address and Give OSD Name
requests from the DUT on its actual wire. The initial test does not require
vendor metadata or qualify every optional CEC discovery feature. It derives
the DUT's initiator addresses from wire traffic, since the UI's aggregate
address fields need not describe the owner-local CEC engine.
It closes the CEC session and logs out on success or failure. Other DUT wires
are untouched. The common runner writes private JSON reports under ignored
`test-results/`. Each report entry has `test`, `passed` and `testbeds` fields;
the runner aggregates preparation, scenarios and successful reset cleanup as
described above. Each test bed contains its DUTs, components, scan evidence,
CEC records and cleanup outcome. A failed run also includes an error. Test
functions need no report-writing code.

Alternatively, keep HTTP authentication and credentials on the development
computer and run Matter enrollment, CEC I/O and physical button control over SSH
on the Pi:

```sh
python3 cec-harness/run.py \
  --credentials /private/local/path/amity-dut.json --insecure \
  --harness-host pi@test2.local \
  --output test-results/functional/hdmi-scan-framework.json
```

Run [Pi software setup](#pi-software-setup) first; its default deployment matches
the runner's `/home/pi/amity/esp32/cec-harness` location. Set `--harness-root`
for another installation. `--ssh-port` supports an existing SSH tunnel. SSH uses the
normal known-host and authentication configuration. The harness transport
carries CEC frames, component identities, bounded physical button requests and
Matter enrollment settings when an explicit credential file is supplied;
it receives no DUT HTTP password or session token. This is harness IPC, not an
additional firmware API. Remote kernel timestamps are mapped conservatively to
the local monotonic clock so
queued pre-scan frames cannot count as fresh discovery evidence.

## Interactive use on test2

This example inspects an enrolled main retained after a failed run. Direct
`DUT` and `TestBed` calls do not perform the runner's enrollment or final reset.
Use the Pi's ordinary Python 3 for these HTTP/CEC helpers; their imports do not
need the Matter SDK on `PYTHONPATH`. Start in the framework directory so its
package is importable:

```sh
cd /home/pi/amity/esp32/cec-harness
python3
```

At the prompt, authenticate interactively and keep a test bed open while you
inspect or control the DUT and components:

```python
from contextlib import ExitStack
from getpass import getpass
from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent

dut = DUT("amity-dut", insecure=True)
dut.login(getpass("Amity admin password: "))
tentacle = dut.tentacle(0)
tv = TvComponent(tentacle.front, "Test TV")
avr = AvrComponent(tentacle.back, "Test AVR")
player = PlaybackComponent(avr, "Test Player", port=1)

stack = ExitStack()
bed = stack.enter_context(TestBed(dut, tv, avr, player))
scan = dut.scan_hdmi()
_ = scan.assert_contains(tv, avr, player)
```

The password prompt does not echo or save the password. `insecure=True` accepts
this bench's self-signed HTTPS certificate. Wiring lookup and CEC setup are
automatic. When finished, run `stack.close()` to release the CEC adapters and
log out. This closes fixture resources without factory-resetting the main.
When debugging is complete, explicitly reset it before the next scripted run.
Scripted scenarios use `with TestBed(...)` for fixture cleanup; the common
runner separately owns initial enrollment and success-only final reset.

Host-only checks from the checkout root:

```sh
PYTHONPATH=cec-harness python3 -m unittest discover -s cec-harness/unit_tests -v
```

These validate the framework, not DUT behavior.
