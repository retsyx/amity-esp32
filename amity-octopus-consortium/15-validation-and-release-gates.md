# Validation of original behavior through owner delivery

The acceptance target is original local Amity behavior with qualified ownership. A build, Ready endpoint, retained fields or successful transport handshake cannot establish that target. [AGENTS.md](../AGENTS.md), [source baseline](19-source-baseline.md), [CEC behavior](06-cec-roles-and-message-policy.md) and [execution](07-activity-planning-and-execution.md) apply.

## Behavioral comparison

Feed identical activities, observations and input events to baseline `98e64a6`, qualified all-local handling and qualified remote-owner handling. Compare emitted wire, initiator, destination, payload, ordering, attempt budgets, admission results and key lifecycle. Transport timing is not permission to change original cadence, timeouts, discovery/recovery or queue behavior.

Required cases:

- Selective power transitions and same-target repair, one device used in several roles, TV/source power differences, standby and original completion-paced activity-settling/input readiness.
- Separate AVR/audio equipment, source routing, missing source with explicit switch-device/input fallback and non-CEC source use. Power/key work reaches the owner even with no coordinator observation; source presence comes from owner lookup, and FIFO overload must not masquerade as absence.
- Missing owner/adapter/OSD name and later return with zero activity-enable/configuration mutation; identical OSD text on separate owners/adapters/sides.
- Per-adapter original allocation, back-first discovery, lowest usable detected front PA or saved fallback, fixed-for-boot selection, received-frame effects and discovery-triggered original handling.
- Independent simultaneous keys, completion-paced repetition, new input during outstanding ACK, finite taps, aggregate recipient release, failed-key termination and matching late-up no-op.
- Eight total active/queued CEC messages per wire, immediate final up, permanently dropped full-FIFO release, every dropped TX logged and no outgoing executor/retry backlog.
- Original startup/accepted-selection wait-for-release gate, swallowed hold behavior, Siri-only Power macros, unchanged swipe/tap recognition and generated event sequence.

Use meaningful transcript/integration assertions. A test that merely expects new graph-policy types or repeats the implementation is not evidence that original behavior was preserved.

## Persistence and migration

Canonical schema 3 contains selectors and user intent, with no enabled, executable route, binding/domain/resource/capability/PA approval graph or durable activity-resume ledger. Migrate v1/v2 while retaining stable IDs where present, all role/switch fields, order, slots and aliases. Ignore old enabled flags. Inject storage cuts and verify backup/identity/trust/journal consistency without restoring runtime addresses, input or grants.

Verify temporarily absent equipment remains selected, AVR/switch activities remain executable through original handling and no new repair/approval workflow is required. The prior six-retained/four-disabled migration is a regression case, not successful parity evidence.

## Transport and security

Test current exact owner/controller membership, boot/link/config/input context, sequence/replay rejection, bounded IPC and stale messages. Locally and remotely delivered original intents must use the same executor behavior. Auth rejection and state cleanup must not redirect a release or restart a failed key.

Retain direct enrollment discovery/physical-window, encrypted ephemeral admission and optional printed-Matter-code PAKE with profile-mismatch rejection, exact-leaf TLS, durable original role/headless boot, settings generation/atomic-save/response-attempt restart, explicit removal/reset and exact-image update checks. Those are separate management/security contracts and do not justify persisted HDMI route approval.

## User interface

Preserve approved tab/form/layout behavior and original activity controls. Device choices include actual owning hostname and stable adapter context while carrying complete qualified identity. Missing choices stay saved. There is no Enable/Approve-path control, disabled import workflow, receiver-profile wizard or member UI. Verify backend dispatch as well as rendered controls.

## Physical qualification

Host/ASan/UBSan tests establish software behavior within their fixtures. Separate hardware evidence must cover actual ESP32-to-ESP32 transport, real Siri/Atomic input, local/remote CEC wire captures, all configured adapter pairs and simultaneous load, radio/HTTPS/scan pressure, full FIFO and loss/reconnect, detachable hardware and real AVR/display/source picture/audio. CEC ACK alone is not AV success.

Preserve existing production gates for secure boot/storage, release signing, provisioning/recovery and ecosystem pairing retention. Current four-adapter profile and three-device software limits are not measured product-capacity promises.

## Functional test framework

The configurable Python framework is in [`cec-harness/`](../cec-harness/README.md).
It parses the physical DUT/tentacle/side GPIO assignments from
[`TEST-HARNESS.md`](../cec-harness/TEST-HARNESS.md), creates virtual components for
each scenario and clears their CEC addresses on exit. The former fixed virtual
theater profiles, boot-time follower setup and previous hardware scripts have
been removed.

Functional files declare equipment, topology and behavior only. The initial
concise test constructs `DUT("amity-dut")`, a TV, AVR and player, then uses
`with TestBed(...): dut.scan_hdmi().assert_contains(...)`. DUT construction
implicitly reads the wiring document. Shared configuration, test-bed, runner
and discovery modules own CLI options, per-DUT credentials/URLs/TLS,
authentication and identity checks, cleanup, UI operations, evidence capture
and scan assertions. The resource interface supports multiple DUTs without
adding another functional scenario. Component protocol `handle()` methods
remain in the common framework. Generic private reports contain `test`,
`passed` and a `testbeds` list; tests contain no reporting helpers.

HDMI attachments explicitly include the DUT: TV on `tentacle.front`, AVR on
`tentacle.back`, playback on an AVR input. A child inherits its parent's owner,
tentacle and CEC wire. The inline DUT adds no physical-address level; a TV-input-1
chain gives TV `0.0.0.0`, AVR `1.0.0.0` and player `1.1.0.0`. Pairing the HDMI
endpoints for address assignment preserves their separate CEC wires. Reports
identify actual endpoint or component attachments alongside the derived GPIOs.
The scenario explicitly asserts that the scan includes exactly TV, AVR and
playback, rejecting extra or duplicate entries. It then checks typed reported
connections: TV on tentacle 0/front, AVR and playback on tentacle 0/back. These
assertions correlate the existing UI device selectors with qualified equipment
observations; fixture wiring or display labels alone are insufficient evidence.

Functional DUT state access MUST use the existing authenticated shipping UI
HTTP routes. Do not add firmware APIs, test-only commands or authentication
bypasses for functional testing. GPIO button control uses the existing physical
input; virtual component power, hooks and CEC emulation live on the harness.
This adaptation changes no DUT discovery, activity, input or CEC policy.

Each isolated GPIO is a virtual CEC bus as well as a physical DUT wire.
Directed component exchanges and replies route synchronously within that bus;
powered targets ACK, Off targets NACK without RX/hooks, and polls report
presence without RX handling. Self-directed traffic does not ACK. Different
GPIOs remain isolated regardless of topology parent links. Broadcast delivery
to virtual peers and transmission to the DUT wire are independent; return the
actual wire result and call `after_tx` once. Hooks may send nested replies,
with a 32-level delivery bound rejecting echo loops and no added queue or delay.
Physical Linux adapter restrictions apply to the wire backend, not to virtual
component communication. TX/RX records distinguish `virtual` and `wire`;
virtual records have `wire_timestamp_ns=0` and cannot establish DUT behavior.

The framework contains four functional scenarios. `hdmi_scan.py` has `amity-dut`
scan a TV, AVR and playback device. It requires fresh wire identity exchanges
and matching UI names/types/addresses. Wire captures identify the actual DUT
initiators; legacy aggregate UI address fields are not owner-local addressing
evidence. Freshness compares the actual wire bytes: operand-free name/address
queries, the expected OSD-name reply, and the expected physical-address/device-type
reply. Cached matching UI rows cannot substitute for those identity operands.
Host regressions keep matching UI observations while rewriting reply identities
and require the complete scan assertions to reject them, including a real session
TX hook. The strengthened assertions also pass in the 29 October bench suite below.
The earlier exact-set and typed-port scan assertions passed on firmware
`0.1.84` on 28 October 2026; private evidence is in
`test-results/functional/hdmi-scan-exact-ports.json`. This does not replace the broader
behavioral comparison or physical qualification gates above.

`activity.py` reuses that topology and all exact scan assertions. It creates a
temporary activity through the existing authenticated UI editor, checks that
all components remain in Standby before Start, then requires a fresh physical
DUT-origin power-on command and On state for each component. Standby must send
a fresh CEC Standby command to each component and leave all three in Standby.
Off remains the separate no-power/no-CEC model. Local state assignments,
virtual exchanges, stale frames and UI admission alone cannot establish those
effects. Registered cleanup runs before CEC closure/logout and removes only
test-owned configuration while preserving other activities and normal history.
No firmware API or original activity/CEC behavior changes are authorized by
this scenario. The runner selects `hdmi_scan`, `activity`, `source_theft`,
`add_amity` or `all`.

`source_theft.py` extends the fixture with a second playback device on AVR
input 2. Both players have saved activities using the same TV and AVR. After
starting and verifying the input-1 player's activity, the input-2 player wakes
and broadcasts Active Source. Require a fresh directed DUT Standby to that
player, a fresh DUT Set Stream Path restoring the input-1 player's PA, the
original player's active/On state, and the same selected activity and boot.
The second playback model explicitly uses the tuner allocation pool while
retaining playback identity and handling. The three-component back wire uses
CEC 1.4; the front TV and original legal two-component fixtures retain CEC 2.0.
Routing proof rejects virtual, stale, rewritten, dropped and wrong-source
frames, and waits for the entire component delivery to finish within its
deadline. Both temporary activities and their unreferenced equipment must be
cleaned up before CEC address release.

Validation on 28 October 2026: all 161 framework unit checks pass. On the
`test2` harness against `amity-dut` firmware `0.1.85`, the standalone scan passes,
and the activity test confirms initial Standby plus fresh physical power-on
commands and On state for all three components. Returning to Standby exposes
a reproducible firmware failure: the `amity_hub` task overflows its stack and
the DUT restarts before the components receive CEC Standby. The `0.1.85` test
run therefore failed at this step; its assertions have not been relaxed. Private
evidence is in `test-results/functional/activity-suite-0.1.85.json`,
`activity-0.1.85-uart.json` and `activity-dut-uart.log` under the same directory.
After the old-boot credentials correctly failed cleanup, fresh authenticated
recovery removed only the confirmed test-owned activity/equipment and preserved
the original two equipment records and activity list. All 14 GPIO CEC adapters
were verified to have zero logical-address claims. This established the test's
ability to detect the `0.1.85` failure.
An isolated Xtensa compile with the current firmware flags and stack-usage
reporting accounts for at least 6,304 bytes along the nested Standby ownership
dispatch path before final transmission/runtime overhead, exceeding the
task's former 6,144-byte allocation. Object call relocations retain these
frames; no recursive cycle was found. Firmware `0.1.86` increases the shared
`amity_hub` task stack from 6 KiB to 12 KiB to accommodate that ownership layer.
This adds 6 KiB of internal RAM for the single shared task; no CEC algorithms,
frame order, attempt budgets, input handling, task priority or cadence change.

With `0.1.86` installed on `amity-dut`, the unchanged scan/activity suite and two
additional activity cycles all pass on `test2`. Every cycle confirms all three
initial Standby states, fresh physical power-on commands, and fresh directed
CEC Standby (`0x36`) commands with final Standby states. The DUT retained one
boot ID throughout all three cycles. Its original activity list and two
preexisting equipment records are preserved; temporary configuration was
removed and all 14 GPIO CEC address masks are zero. Internal free heap was
32,283 bytes after the update and 32,015 after the three cycles, with an
11,264-byte largest internal block at both observations. Runtime stack
high-water margin was not measured. The targeted CEC/hub and owned-hub suites
pass with ASan/UBSan, and the pinned native firmware build passes. Evidence is
in `test-results/standby-stack-fix-0.1.86/`, including `functional-suite.json`,
`activity-cycle-2.json`, `activity-cycle-3.json`, `verified.json` and
`adapters-after.json`. This qualifies the reproduced local standby failure;
four-adapter electrical load, remote radio delivery and real AV behavior retain
their separate qualification gates.

The subsequent `OwnedIntent` reference refactor passes the CEC/hub, owned-hub,
actual activity-facade and owner-synchronization host suites with ASan/UBSan.
The owned-hub regression compares complete Start/Standby frames, order and
attempt budgets for original local, qualified local and two-owner equipment.
It also enqueues On and Standby requests, destroys the caller's intent and
authorization before the worker runs, and verifies the original frames are
still emitted. The command owns its payload; synchronous forwarding borrows it.
Logs are `test-results/functional/owned-intent-refs-{cec,owned,control,sync}.log`.
Before/after Xtensa stack-usage records for the same optimized Standby call
chain show a known subtotal of 6,112 versus 3,968 bytes, excluding final
owner-send/CEC/runtime frames. These static measurements do not establish a
runtime stack high-water margin or qualify this refactor on physical hardware.

Validation on 29 October 2026: all 204 framework unit tests pass, including
physical/virtual provenance, alternate allocation profiles, CEC-version
reporting, bounded peer-delivery completion, two saved activity identities,
and cleanup under injected backend failures. On `test2` against `amity-dut`
firmware `0.1.88`, all three functional scenarios pass. The source-theft case
claims back-wire addresses 5/4/3 in CEC 1.4 mode, configures both activities,
and verifies fresh DUT frames `03:36` and `0f:86:11:00` after the second
player's announcement. The original activity remains selected on the same
boot. Final authenticated snapshots preserve the saved activities/equipment,
return to idle, and all 14 GPIO CEC address masks are zero (the two native Pi
HDMI adapters were also checked). Evidence is
`test-results/functional/source-theft-framework-tests.log`,
`source-theft-suite-0.1.88.json` and `source-theft-verified.json` in that
directory. This is one local DUT/tentacle with simulated CEC equipment, not
qualification of real AV switching, remote owner radio delivery or all adapters.

`add_amity.py` uses the original `amity-dut` and a control-only `amity-dut-2`.
Only the main authenticates to HTTP. The candidate's 3.5-second FW RESET hold
uses the setup/reset GPIO parsed from `TEST-HARNESS.md`, with Pi-owned timing
and release when running over SSH. Existing UI scan/add/operation/settings
calls must prove the full candidate ID, requested hostname, connected ready
membership and configuration adoption while retaining the original main and
existing saved intent. Cleanup uses only the proven new member's reviewed
membership revision and boot; Remove must acknowledge its scheduled factory
reset. A failed search never authorizes resetting an unrelated board.

All 238 framework unit tests pass, including GPIO interruption/release, strict
candidate identity, control-only ownership, uncertain-reply reconciliation,
request-report redaction and Markdown-derived pin fixtures. The physical test
does not pass: the first attempt hit a TLS handshake timeout during discovery;
the admission-only polling retry remained responsive but found no candidate.
Candidate firmware `0.1.84` logged a `fw_reset` task stack overflow and software
reboot during the attempt. Neither attempt submitted Add or Remove, and their
own scans were canceled. Connected membership and reset-on-removal remain
unqualified. Diagnostic serial opening also caused unintended USB resets on
both boards; capture was stopped. Main configuration/identity remain intact,
with two current Matter fabrics confirmed through the existing UI. Evidence:
`test-results/functional/add-amity-2026-10-29.json`,
`add-amity-retry-2026-10-29.json`, `add-amity-retry-amity-dut-2-uart.log`,
`add-amity-after-2026-10-29.json` and `add-amity-main-matter-after.json` in that
directory. No firmware or assertion criteria were changed to hide this result.

## Evidence record

Record exact source commit, build/configuration identity, SDK/dependency pins, test commands/results and artifact hashes. Label historical graph-model tests as historical; do not use them to claim the new selector model is tested. Update [VALIDATION.md](VALIDATION.md) and `IMPLEMENTATION_STATUS.json` only with checks actually run. The current task's final result must distinguish software/build evidence from physical device testing.
