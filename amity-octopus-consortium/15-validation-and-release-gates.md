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

## Persistence and format validation

Canonical schema 3 contains selectors and user intent, with no enabled, executable route, binding/domain/resource/capability/PA approval graph or durable activity-resume ledger. Accept only the current numeric format version; reject missing, earlier and future versions and obsolete fields. Exercise direct empty-current-format initialization and retain role/switch fields, order, slots and aliases across ordinary current-format edits. No migration runs. Inject storage cuts and verify backup/identity/trust/journal consistency without restoring runtime addresses, input or grants.

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
Both Standby and route restoration must complete within 250 ms of the same
successful physical Active Source transmission, using complete TX/RX wire
timestamps. The two-owner and three-owner consortium cases apply the same
250 ms limit to directed Standby on each isolated thief wire. Slow HTTP owner
proof cannot extend or restart the physical deadline; larger operation or
ordinary CEC timeouts cannot raise it.
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
directory. Those attempts changed neither firmware nor assertion criteria to hide the result.

The 0.1.88 rerun also failed discovery: 40 successful admission polls remained
`scanning` until the 90-second deadline; the final short-deadline HTTPS request
reported a handshake timeout. No Add/Remove was submitted. Cancel, parsed-pin
release and exact main boot/configuration/authority/two-fabric preservation
checks passed. Target analysis identifies a setup-button stack defect, and the
prepared 0.1.89 source separated manual Matter setup and increased that task to
6 KiB without changing gestures or radio ordering. At that stage it had not
been deployed or qualified; later debug results below retain the failed runs
and distinguish gesture evidence from completed membership/removal. Current
evidence and the retained earlier failure are recorded in `VALIDATION.md` and
`IMPLEMENTATION_STATUS.json`.

### Add debug corrections — 29 October 2026

Candidate 0.1.89 subsequently opened its Add window without the observed stack
overflow; the actual 3.5-second gesture left 3,968 bytes of minimum free task
stack. That measured one exercised setup path, not every possible SDK stack
peak. The original functional attempt still failed. Main 0.1.90 explicitly
negotiates ATT MTU before record transfer: the default 23-byte MTU had left only
ten payload bytes per fragment, requiring 195 fragments for the observed
proposal and exceeding the unchanged 30-second assembly limit.

Candidate 0.1.91 keeps the existing 35-second network budget across the driver
request, association callback and usable IPv4/DHCP wait. The earlier callback
reported association before an IP address and triggered rollback. Framework
verification now polls the existing device-settings endpoint directly while its
two-second reply cache is live, then repeats the full boot/authority/config/
membership/name checks. Its overall deadline and required assertions remain
unchanged; all 240 framework unit tests pass.

The remaining physical Remove attempt failed with a settings reply already in
flight ahead of the retirement acknowledgment. Main 0.1.92 discards only
exact-scope, capability/TTL/replay-valid settings results (as well as permitted
heartbeat/lease traffic) within the existing lifecycle deadline and 32-frame
bound. It restores lifecycle restriction immediately and never applies those
replies. Dedicated candidate reset/recovery succeeded after that failed attempt;
it is not a passing functional Remove acknowledgment.

The final 0.1.92 native build passes all 35 public-record layout checks, and
`tests/run_host_tests.sh` exits 0 including all 240 framework unit tests. Logs
are `build-0.1.92.log` and `full-host-tests.log` in the debug directory. The
main OTA to the final image passes all 13 exact-image/preservation checks
without main USB access. The unchanged original test on main 0.1.92/candidate
0.1.91 completes Add and exact connected-member/settings checks, but remains
FAIL during Remove: the operation completes with reset `unconfirmed`, and the
candidate logs flash-cache stack-safety and later semaphore assertions. It does
not return to proven unclaimed startup; scheduled reset acknowledgment remains
unqualified. The report is `add-amity-main-0.1.92.json` with
`add-amity-main92-instrumented.json` and `candidate-during-main92-test.log` in
the same directory. No new firmware API, weaker assertion, CEC policy or longer
transaction deadline was introduced.
Private intermediate failures, compiler records, deployment checks and candidate
reset recovery are retained under `test-results/add-amity-debug-2026-10-29/`.

The captured 0.1.91 assertions were then traced through matching instruction
windows and source: `esp_restart` reached the diagnostics NVS shutdown write on
the PSRAM link stack, and headless retirement attempted mDNS removal without a
registered responder. Candidate 0.1.93 uses the existing internal-flash-stack
broker after the unchanged acknowledgment/500 ms restart delay, and guards owned
administration mDNS lifetime before Matter shutdown. Its app-only deployment and
separate reset recovery pass; neither is a functional Remove result.

Main 0.1.94 retains Add completion in the existing transient operation view only
after the exact selected peer proves ready and correctly named. Later removal
does not undo that finished result; current liveness still requires live checks.
The 0.1.93 native/full-host and 0.1.94 native 35-layout checks pass, and the exact
0.1.94 main OTA preserves its configuration, two fabrics, authority, HomeKit and
capabilities. The final `verified-host-tests.log` exits 0 including 240 framework
unit tests. Both consecutive unchanged original 0.1.94/0.1.93 functional runs
pass Add, both same-boot connected/settings assertions, and cleanup once with
reset `scheduled`, no retained member and no cleanup errors. Candidate UART
shows unclaimed Ready and automatic Matter setup after each reset, no panic or
Backtrace, and minimum free setup-task stack of 3,968/3,976 bytes. Final main
HTTP checks preserve the same boot, group/authority, exact configuration and two
fabrics, with ready idle state and no candidate. Reports are
`add-amity-main-0.1.94.json`, `add-amity-repeat-main94-candidate93.json`,
`add-amity-main94-instrumented.json` and `add-amity-main-0.1.94-after.json` in the
debug directory. Main USB was not opened. This is normal-profile Add/removal
qualification for this pair; other boards remain 0.1.88 and no CEC/AV or strict
setup-code profile qualification is claimed. No new
firmware API, weaker assertion, longer transaction deadline or CEC policy is
introduced. All prior failed reports remain separate failure evidence.

## Evidence record

Record exact source commit, build/configuration identity, SDK/dependency pins, test commands/results and artifact hashes. Label historical graph-model tests as historical; do not use them to claim the new selector model is tested. Update [VALIDATION.md](VALIDATION.md) and `IMPLEMENTATION_STATUS.json` only with checks actually run. The current task's final result must distinguish software/build evidence from physical device testing.
