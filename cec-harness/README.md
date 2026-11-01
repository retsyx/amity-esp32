# Amity functional test harness

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
Additional harness helpers must wrap existing UI operations as well. Firmware CEC, activity, input, commissioning and
membership behavior remains unchanged.

## Layout

- `TEST-HARNESS.md`: bench inventory and wiring; the only GPIO mapping source.
- `amity_harness/`: named DUT control, Markdown wiring parser, topology/address
  assignment, CEC transport and component emulation. Shared `config.py`,
  `testbed.py`, `runner.py` and `discovery.py` own runtime configuration,
  resource/authentication lifetime, CLI/reporting and scan evidence/assertions.
- `functional_tests/hdmi_scan.py`: exact HDMI scan checks and the reusable scanned fixture.
- `functional_tests/activity.py`: creates an activity from that scan, starts it,
  then verifies standby using physical component power commands.
- `functional_tests/source_theft.py`: adds a second playback device on AVR
  input 2 and verifies that Amity restores the first player after source theft.
- `functional_tests/add_amity.py`: adds the second bench board through the main
  Amity, verifies its membership, then removes and resets that added board.
- `functional_tests/consortium.py`: scans equipment on the main and one or two
  headless members, then checks activity power and source-theft suppression in
  every ownership direction.
- `unit_tests/`: hardware-free framework checks, separate from functional tests.

The former `tools/cec-harness/` fixed profiles, setup scripts and systemd service
templates have been removed. The static virtual-device inventory has also been
removed from `TEST-HARNESS.md`. The remaining wiring tables describe physical
connections, not simulated equipment.

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

            stopped = activity.standby()
            for component in components:
                component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
```

`DUT.create_activity()` uses the exact scanned selections and the shipping UI's
authenticated activity editor. It retains existing activities.
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

On 29 October 2026, the three CEC functional scenarios passed on `test2` against
`amity-dut` firmware `0.1.88`, along with all 204 framework unit tests. The
source-theft run verified physical directed Standby `03:36` and route
restoration `0f:86:11:00`, retained the same boot and saved activities/equipment,
and released all 14 GPIO CEC address masks. Private evidence is in
`test-results/functional/source-theft-suite-0.1.88.json` and
`source-theft-verified.json` in the same directory. This exercises simulated
CEC equipment on tentacle 0; it does not qualify real AV behavior or all adapters.

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
It does not factory-reset the main, run a ten-second reset before the test,
or remove a preexisting member. Failures retain/report uncertain ownership
rather than removing another device.

An Add-only physical timing run on 29 October 2026 with firmware 0.1.99 took
95.494 seconds from helper entry to fully verified ready state. The Add POST
to membership-operation completion took 59.272 seconds; Add POST to verified
settings/ready took 67.940 seconds. This was one normal-profile run on
`amity-dut`/`amity-dut-2`, without CEC component traffic, and includes host HTTP
time. Removal acknowledged reset `scheduled`. It measures enrollment and does
not establish a CEC response allowance. Evidence is
`test-results/device-removal-2026-10-29/enrollment-measurement.json` and
`enrollment-timing.json` in that directory.

On 29 October 2026, all 238 framework unit tests passed. Physical validation
of `add_amity.py` failed during candidate discovery: the 3.5-second gesture on
`amity-dut-2` firmware 0.1.84 produced a stack overflow in its `fw_reset` task,
and the main never found the candidate. The harness confirmed button release
and completed scan cancellation; it submitted neither Add nor Remove. Admission,
connected-member checks and the removal/reset acknowledgment remain unqualified.
No firmware was changed and the existing-API assertions were retained. Private
evidence is in `test-results/functional/add-amity-retry-2026-10-29.json` and
`add-amity-retry-amity-dut-2-uart.log` in that directory.

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

On 29 October 2026, all six functional cases passed on `test2` with firmware
0.1.98, including both directions of `consortium.py`. Its scan proved exactly
six typed equipment locations across the four wires; fourteen fresh physical
power-command assertions covered both activity cycles and the two thieves.
Both activities/equipment were removed and member removal acknowledged its
scheduled reset, with no cleanup errors. All four bench DUTs run the same
verified image. Reports are in
`test-results/consortium-live-config-2026-10-29/functional.json` and
`deployment.json`. This qualifies the exercised two-board, one-adapter-per-board
normal-profile scenarios with simulated CEC equipment; historical records above
retain their original scope.

## DUTs and wiring

`DUT("amity-dut")` implicitly reads `TEST-HARNESS.md`.
`Wiring.from_markdown()` finds the **HDMI-CEC harness GPIO mapping** table and
builds `(DUT name, tentacle, front/back) -> Pi GPIO`. It also reads DUT identities
and the setup/reset GPIO table. Duplicate assignments, unknown DUTs, missing
paired sides and overlapping pins are rejected before hardware access.

The runner supplies a scoped `RunConfig` for credential files, DUT URLs, TLS,
wiring, timeouts and CEC transport. The credential JSON contains `admin_password`
and must be private (mode `0600`). The default HTTP origin is
`https://<DUT name>.local`; use `--url` or per-DUT `--dut-url DUT=URL` to override
it. TLS verification is enabled by default: supply `--cafile` for a trusted
bench CA or explicitly choose `--insecure` for its self-signed certificate.
Use `--credentials PATH` for the run's default credentials, or repeat
`--dut-credentials DUT=PATH` for individual DUTs. `--wiring PATH` selects a
wiring document. These settings stay outside the functional test body.
Passwords and session tokens are never included in test reports.

Call `dut.press_fw_reset(3.5)` to request a physical 3.5-second hold; the
firmware interprets the gesture. FW RESET is active low. A hold drives only
its documented Pi GPIO low and releases the pin to input with pulls disabled
in `finally`, including on
interruption. A three-to-below-ten-second hold has the existing Add gesture
meaning; ten seconds has the existing factory-reset meaning. The HDMI scan
test does not press FW RESET, commission, rename, or change saved activities.

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

## Run functional tests

Requires Linux/Pi, Python 3.10 or newer, the existing `cec-gpio` adapters and
permission to open `/dev/cec*` (normally membership in `video`). GPIO button
operations additionally require `pinctrl`. The framework has no third-party
Python dependencies. Stop/remove any previously installed `amity-cec@` follower
services before running it; they must not reclaim the adapters.

From the checkout root **on the Pi**:

```sh
python3 cec-harness/run.py \
  --credentials /private/path/amity-dut.json --insecure \
  --output test-results/functional/hdmi-scan-framework.json
```

The default remains `hdmi_scan`. Select `--test activity` for both the activity
power-cycle and explicit switch-input sub-tests, `--test source_theft` for unselected-source recovery,
`--test add_amity` for adding and removing the second board, `--test consortium`
for the two-owner equipment scenario, `--test consortium_three_duts` for the
three-owner scenario, or `--test all` for all six registered scenarios (seven
test cases). The same credentials, TLS, SSH and output
options apply. A multi-test report has
`passed` and `tests`; each entry keeps the ordinary single-test report format.
`--timeout` defaults to 180 seconds for scan, enrollment and activity operations,
based on the measured 95.494-second verified Add above.
`--cec-timeout` independently defaults to five seconds for ordinary fresh power
and routing assertions. Changing the enrollment operation budget does not change
the CEC budget. Source-theft
remediation has the stricter 250 ms physical window described above,
including both Standby and shared-wire route restoration, regardless of either
option. Owner identity proof still uses bounded HTTP requests and can extend
the caller's wall time while a request is in progress; accepted physical
response timestamps must remain inside their original window.
The shared framework logs progress to stderr as it runs, with a UTC timestamp,
elapsed run time, run ID and test name on each line. It records setup, identity
checks, scans, activity and membership operations, component assertions,
failures and cleanup. PASS/FAIL summaries go to stdout/stderr after each test.
After writing the JSON report, the final `run.report` line gives its absolute
path on successful and failed runs.

Owner-proof failures retain the rejected check's bounded identity, boot,
readiness and proof fields in stdio and `scans[].owner_failures` in the JSON
report. Primary failure and cause stacks survive cleanup failures. Only
framework-owned assertion messages are included; arbitrary exception text,
HTTP bodies, credentials and opaque tokens are excluded.

Use `--log-level DEBUG` to include HTTP method/route, request duration and
status, and individual CEC frames with owner, GPIO and wire/virtual transport.
HTTP bodies, headers, credentials and opaque tokens are excluded. No separate
log file is created; capture standard output and error to retain the trace:

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
`test-results/`. Reports have top-level `test`, `passed` and `testbeds` fields;
each test bed contains its DUTs, components, scan evidence, CEC records and
cleanup outcome. A failed run also includes an error. Test functions need no
report-writing code.

Alternatively, keep HTTP authentication and credentials on the development
computer and run CEC I/O and physical button control over SSH on the Pi:

```sh
python3 cec-harness/run.py \
  --credentials /private/local/path/amity-dut.json --insecure \
  --harness-host pi@test2.local \
  --output test-results/functional/hdmi-scan-framework.json
```

Install this framework at `/home/pi/amity/esp32/cec-harness` on the Pi first, or
set `--harness-root`. `--ssh-port` supports an existing SSH tunnel. SSH uses the
normal known-host and authentication configuration. The harness transport
carries CEC frames, component identities and bounded physical button requests;
it receives no DUT password or HTTP session token. This is harness IPC, not an
additional firmware API. Remote kernel timestamps are mapped conservatively to
the local monotonic clock so
queued pre-scan frames cannot count as fresh discovery evidence.

## Interactive use on test2

Use the Pi's ordinary Python 3; no virtual environment or third-party Python
packages are required. Start in the framework directory so its package is
importable:

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
log out. Scripted tests use `with TestBed(...)` for automatic cleanup.

Host-only checks from the checkout root:

```sh
PYTHONPATH=cec-harness python3 -m unittest discover -s cec-harness/unit_tests -v
```

These validate the framework, not DUT behavior.
