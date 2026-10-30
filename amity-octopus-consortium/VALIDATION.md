# Current contract validation

The current package describes qualified saved intent and original owner-local execution. Removed planner/resource/binding/route approval fixtures are not active alternatives. The 27 October 2026 instruction and migration lesson are recorded in [AGENTS.md](../AGENTS.md).

## Checks and limits

`validate_contracts.py` checks schema-3 saved intent, qualified selectors and separate runtime endpoints, current scoped owner intents, supported management payload references, rejection of obsolete graph/kinds/fields, actual wire-kind registry consistency, hardware-capacity agreement, local documentation links and exact manifest coverage/hashes. Its hardware-capacity-only mode uses the standard library; full validation uses jsonschema/referencing and performs no network/device operations.

Run:

```sh
python3 amity-octopus-consortium/validate_contracts.py --hardware-capacity-only
python3 amity-octopus-consortium/validate_contracts.py
```

A contract fixture passing does not establish firmware execution. Current software and native-build commands/results are recorded in `IMPLEMENTATION_STATUS.json` after they run. Compare baseline local, qualified all-local and remote-owner transcripts for AVR/audio/switch, missing-name return, original discovery/PA, key lifecycle and FIFO8. A Ready endpoint, build or retained JSON alone is insufficient.

## 0.1.105 Add latency and removal correction pending — 30 October 2026

The current target is 0.1.105 / Matter software 106. Latest deployed firmware
remains verified 0.1.104 on all four DUTs. Three avoidable costs are corrected
in source: boot tests only the configured admission profile while explicit
USB/native self-tests retain both; proposal construction reuses exact validated
`Identity`/`Peer` records instead of repeating two certificate signature checks;
and the first known numeric authority-IP attempt precedes blocking mDNS while
retaining pinned TLS and existing failed-route discovery/rotation/backoff.
The first untrusted certificate checks, profile matching, required member
restart, handshake and authority rules remain unchanged. Actual-source crypto,
startup/network and admission workflow regressions pass. They prove the
mechanisms, not a measured physical latency improvement.

The new 0.1.104 Add timing baseline remains failed. Admission completed
53.704 seconds after Add POST; helper entry to first verified readiness was
92.051 seconds. Initial USB monitor attachment caused a candidate reboot
41.879 seconds before the POST, so this is diagnostic timing evidence rather
than clean qualification. Cleanup received `reset=unconfirmed`. The saved
main/candidate logs and immediate snapshot expose a separate race: Remove
admitted terminal retirement under link configuration 196, then its saved-owner
cleanup published local configuration 197 before the link worker sent the
request. The old global-revision equality check rejected that already-admitted
exchange; the candidate received no retirement and closed the link.

The correction retains an immutable admitted scope/revision permit for the
exact pending removal request through its own configuration publication. It
validates original authority, authenticated full link/boot/group/epoch scope,
mutation/digest, target absence and the original two-second reset lifetime
without advancing or restamping the request. Receiver and ACK checks remain.
The actual removal regression reproduces the old failure and passes the new
publication interleaving plus 19 denied stale/foreign/expired permit cases.
These reset-exchange seconds are separate from the current one-second
source-theft response contract.

Baseline evidence is under `test-results/add-latency-2026-10-30/` in
`baseline-add.json`, `baseline-add.log`, `baseline-candidate.log` and
`removal-config-publish-before-fix.log`; main failure captures are
`latency-baseline-failure-{snapshot.json,logs.txt}` under the 0.1.104 TLS-memory
fix directory. Separate post-failure recovery held only the known fixture
`amity-dut-2` reset button for 10.5 seconds and acknowledged release, recorded
in `baseline-physical-recovery.json`. It does not repair the failed report or
establish a pre-test reset requirement.

New evidence belongs under `test-results/add-latency-fix-2026-10-30/`.
The 0.1.105 complete host suite, build, deployment and physical results are
pending. All seven functional cases are planned; no result is claimed for
them. The one-second source-theft contract and its separate 340-unit host pass
remain current; historical physical reports below retain their recorded
two-second limits.

## Current source-theft deadline — 30 October 2026

The current functional contract now requires thief Standby and resulting state
within one second of that component's successful physical Active Source
transmission. Shared-wire route restoration must complete within the same
original one-second window. The requested framework change is complete:
all 340 framework units pass, including exact one-second boundaries and
rejection of a response one nanosecond late. Command:
`PYTHONPATH=cec-harness python3 -m unittest discover -s cec-harness/unit_tests -q`;
the 14.088-second run is recorded in
`test-results/source-theft-one-second-2026-10-30/framework-tests.log`.
Physical tests were not rerun for this framework-only change. The prior results
below retain their then-current two-second contract and recorded 2,000 ms
bounds; their observed latencies do not retroactively establish a run under
the new contract. The earlier complete 0.1.104 host suite remains the separate
339-unit result recorded below.

## 0.1.104 deployment and physical qualification — 30 October 2026

All four DUTs run exact 0.1.104 / Matter software 105 from firmware commit
`7d8508ed6bef5055b48fab865681d84456e685db`, with `source_dirty=true` as recorded
in the frozen release. The 2,436,688-byte image has SHA-256
`382022e2ed4ae473cbcd2254129a77339c702209bbc03705b78690e1f4981689`;
ELF SHA-256 is
`4072b48bba28d6dca717a20519f13dc242c9893fe8e31022d16c6c6ca3b2430f`.
Authenticated application OTA on the main passed 13 preservation checks without
USB. Each candidate passed 18 checks after a MAC-locked application-only USB
update without erasing flash. The complete host suite, 339 framework units,
native build with 36 public-record layout checks and contracts passed. Evidence
is under `test-results/consortium-tls-memory-fix-2026-10-30/`.

Live shipping-UI verification passed 29 checks: all 11 response bodies completed,
four assets matched the frozen build exactly, large JSON and log responses
completed, and status/security headers and main saved configuration/pairings
were preserved. `web-verification.json` records these single-attempt checks.

All three physical cases passed with unchanged assertions and deadlines:

| Case | Duration | Fresh physical evidence | Cleanup |
| --- | --- | --- | --- |
| Three-owner consortium | 620.437811 s | 24 power assertions, nine typed scan locations, three initial routes and all six cross-owner theft directions; directed Standby responses 66.900010–66.950101 ms | Three activities removed; member 3 then member 2 removed with acknowledged scheduled resets |
| Original two-owner consortium | 348.054372 s | 14 power assertions, six typed locations, two initial routes and both theft directions; responses 67.049612 and 66.950095 ms | Two activities removed; the known added member removed with acknowledged scheduled reset |
| Shared-wire source theft | 135.288419 s | Four typed locations, three fresh start power commands, thief Standby at 66.899968 ms and selected-route restoration at 183.349865 ms from the same physical trigger, both within two seconds | Two activities removed; three fresh physical cleanup Standby commands |

The reports are `consortium-three-duts.json`, `consortium.json` and
`source-theft.json`; their corresponding `*-verification.json` files pass
21, 21 and 20 independent saved-report checks. Every owner was exercised as
active, with the selected UI activity and main boot retained through theft
remediation. No owner-proof or testbed cleanup errors remained. No standalone
Add case was run on 0.1.104: normal Add and exact connected/settings checks are
included in the consortium cases; the separate 0.1.103 Add pass remains below.

The final authenticated UI postcheck passes all 26 checks in
`main-final-postcheck.json`. The two consortium cases together account for
38 fresh power assertions, 15 typed locations, five initial routes, eight theft
directions and three acknowledged reset removals. Main boot
`59c89aa149f680bf357f9a4d220b5dcb` remained stable. Original saved values,
existing slot metadata, names, hostname, installation/group, authority, hardware
capabilities, Matter fabrics and HomeKit pairing state were preserved except
for authorized fixture cleanup and revision changes. The main is ready and
idle, with no enrolled peers, removed-owner observations, pending configuration,
discovery, activity error or hardware error. The snapshot still retains a
nonempty membership diagnostic and cumulative TX errors (129 before the cases,
4,035 afterward); RX drops remain zero. These passing checks do not assert the
absence of transport or CEC transmission failures. The membership diagnostic
is set by an initial handshake/establishment failure and survives later
successful establishment. Its exact cause is outside the retained log window;
the remaining window contains only the expected member-retirement close.

This qualifies the exercised normal admission profile with main `amity-dut`,
added `amity-dut-2` and `amity-dut-3`, one adapter per owner, isolated front/back
wires and simulated TV/AVR/player CEC responders. It does not qualify four
owners, adversarial radio/reconnect/loss/load behavior, the strict admission
profile, four-adapter electrical timing or real picture/audio equipment. The
0.1.103 HTTPS failure, 0.1.102 bench interruption and all earlier failed
physical reports remain failed historical evidence.

## Historical 0.1.103 HTTPS memory failure and correction — 30 October 2026

All four DUTs were deployed and verified on exact 0.1.103 / Matter software 104
from firmware commit `1680c85781b4e310d7a163d4cbc9326ec908caff`, with
`source_dirty=true`. The 2,436,624-byte image has SHA-256
`9d99ea1bde67647b33f3d83a251bd0c645292240db5123a045f200c050648890`;
ELF SHA-256 is
`f0424e1ceacdf4c634bf51f8da0c383f35d3b95527d52e0cef53bfdecba74fe8`.
Main used authenticated application OTA without USB, passing 13 preservation
checks. Each of the three candidates passed 18 checks after a MAC-locked
application-only USB update without erasing flash. The complete host suite,
339 framework units, native build with 36 public-record layout checks and
contracts passed. Evidence is under
`test-results/consortium-empty-context-fix-2026-10-30/`.

The physical Add case passed in 111.719 seconds including cleanup. It verified
the exact member identity, boot, ready state and settings, then removed only the
known added member with a fresh reviewed request and confirmed scheduled reset.
No cleanup errors remained. Independent saved-report verification passed all
20 checks in `add-amity-verification.json`. Measured from same-run monotonic
trace events, helper entry to first verified readiness was 87.832 seconds;
Add POST to operation completion was 50.421 seconds and to first verified
readiness was 59.855 seconds. These intervals include host HTTP time.

The three-owner run failed and remains failed. All nine identities and typed
owner/adapter/side locations passed over six isolated wires with three proven
owner boots. After saving the main activity, `GET /api/system` returned a
truncated response while preparing the second activity, before that activity's
POST. No activity power, routing or theft stage ran. The failure report is
`consortium-three-duts.json`; the separate
`consortium-three-duts-failure-analysis.json` passes 16 analysis checks while
retaining `functional_passed=false`. All fixture activity cleanup completed,
both members were removed with acknowledged scheduled resets, and cleanup had
no errors. The immediate snapshot retained the main boot, image, hostname and
unrelated configuration, except authorized cleanup and revision changes.

Later inspection of `three-owner-failure-immediate-logs.txt` identifies the
HTTPS write failure: `esp-aes` failed to allocate memory, followed by TLS write
error `-0x0084` at the same firmware timestamp. For PSRAM input and output, the
pinned SDK's hardware AES path allocates needed DMA bounce buffers of
`min(1600, length)` bytes each. The 1,600-byte value is a cap, not a minimum;
a 512-byte write bounds each required bounce allocation to 512 bytes. General
free PSRAM does not establish that the required internal DMA memory is
available. The physical log does not identify which allocation or size failed;
`-0x0084` is a generic mapped write error rather than a memory-specific code,
and the framework did not retain the exact physical response cutoff. This
later log diagnosis extends the original saved failure analysis, whose
interpretation left the connection-closure cause unresolved.

The accepted correction targets 0.1.104 / Matter software 105. Shared
`web::response` sends bodies larger than 512 bytes as HTTP chunks of at most
512 bytes and sends smaller bodies directly; JSON, assets and logs use that
helper. Peer TLS writes likewise use chunks of at most 512 bytes, retaining
the existing three-second send budget and WANT handling. Global TLS output
capacity remains 4,096 bytes and input capacity remains 16,384 bytes, preserving
certificate handshake capacity and authentication. Receive semantics,
timeouts and functional assertions are unchanged. Actual-source host regressions
exercise the SDK's HTTP framing, TLS write loop and AES bounce sizing with
controlled allocator/device seams: the old response and peer paths fail the
same assertions, while bounded writes preserve exact bytes and propagate
failures. The modeled 1,024-byte DMA block limit is not a measurement of the
physical failure. Targeted deadline, authentication and production-profile
checks also pass. Results are in `bounded-application-write-regression.json`
under `test-results/consortium-tls-memory-fix-2026-10-30/`. The subsequent
0.1.104 build, deployment and physical results are recorded above. They do not
change this 0.1.103 three-owner failure or the failed 0.1.102 bench attempt;
the 0.1.103 Add and scan evidence alone did not qualify activity or source-theft
behavior.

## Consortium activities and live discovery — 29 October 2026

Firmware 0.1.98/Matter software 99 adds the two-Amity functional scenario in
[`consortium.py`](../cec-harness/functional_tests/consortium.py). Main
`amity-dut` and member `amity-dut-2` each have a TV on adapter 0/front, an AVR
on adapter 0/back and a player behind AVR input 1. Both activity roles are
exercised. The scan asserts exact membership and six typed owner/adapter/side
locations. Fourteen fresh physical power-command assertions cover start,
directed source-theft suppression and standby, with AVR routing and retained
active-source checks. Virtual traffic cannot satisfy those assertions.

Activity saves preserve member TLS connections and HDMI caches. Only boot,
explicit UI requests and required-device cache misses initiate enumeration.
`Hub::find_device` alone owns cached lookup, per-owner/adapter/side two-minute
miss throttling and the subsequent lookup. Valid announcements update the
cache independently of saved configuration. Source policy uses the current
activity, including global standby, without an owner inactive flag. An activity
still in flight is not active: an early announcement may legitimately receive
Standby, and the normal activation sequence later powers that device on. No
pending-context timing exception or saved-selector cache-loss workaround remains.

Activity start and standby steps execute sequentially within each Amity, waiting
for each lookup and admitted transmission before the next step; different
Amities may progress concurrently. The original wait-for-release input gate
discards presses during setup before pressed-state registration. Unregistered
releases are no-ops; preexisting key cleanup, normal independent keys and the
ungated long-Power repair remain. Baseline comparisons use Python Amity's
`ControllerImpl.get_device`, sequential activity methods and CEC completion
behavior. Host regressions exercise actual Hub, HubService, SystemSync and
Session code, unsolicited device announcements, live revision changes over one
TLS link, owner-local source policy, scan throttling and frame ordering.

The complete host runner passes with 287 framework units and the native build
passes 36 public-record layout checks. Evidence is private under
`test-results/consortium-live-config-2026-10-29/`: `host-tests-complete.log`,
`build-complete.log` and `contracts-complete.json`. The frozen application in
`firmware-0.1.98-final/` is 2,414,032 bytes in the 3,997,696-byte slot (40% free),
SHA-256 `383ceed1c39eacc6423fc893361343a828f78bdb04616ebbe20f8726ff94f560`,
ELF SHA-256 `de300e3e6b7c4ec75b5eaa9a935ca44e307e78c914ae08260efa11216d3f5739`.
Its provenance remains parent `1be1436` with `source_dirty=true`; the later
change commit does not make this a clean committed-source build.

All four DUTs run that exact image. Main used existing authenticated application
OTA, passing 13 preservation checks, without USB access. Boards 2/3/4 used
MAC-locked application-only USB writes, full backups and protected-flash
comparisons before reboot; each passes 18 checks, including factory identity,
automatic Matter setup and the SDK admission self-test. Updates used no full
erase or factory reset. `deployment.json` and the four individual deployment
reports retain the evidence. The intermediate 0.1.97 deployment and failing
consortium attempts remain separately recorded, not relabeled as passes.

Final `functional.json` passes all six cases: HDMI scan, activity, explicit
switch input, source theft, Add Amity and consortium. Every testbed passes with
no cleanup errors. The consortium run removes its two activities and six
equipment records before removing the member; removal completes with reset
scheduled and no retained member. Final `main-final-postcheck.json` passes 23
read-only checks and logs out. Main retains its post-OTA boot, original
authority, all saved values except normal revision metadata, two Matter fabrics,
HomeKit state and all sixteen preexisting reserved slots. No new slots or
active fixture peers remain. An initial read-only verifier retained the previous
task's thirteen-slot/three-new-slot expectations; its failed report is preserved.
Correcting those baseline expectations required no DUT mutation.

Physical scope is normal admission and distributed activity/source suppression
on these two Amities with one adapter each and simulated HDMI-CEC equipment.
Three-owner operation, strict-profile enrollment, real AV/remote behavior,
four-adapter electrical/load timing and production security remain separate
qualification gates. No new firmware API or functional-test control surface was
introduced. Permanent behavior requirements are recorded in
[AGENTS.md](../AGENTS.md).

## Explicit Remove and direct Replace — 29 October 2026

Firmware 0.1.99/Matter software 100 changes confirmed Remove to delete the
selected owner, its equipment/settings/name and every referencing activity.
The dialog lists activities across all four roles and confirms their exact IDs
and configuration/presentation revisions together with membership revision and
target boot. Offline-to-known-boot changes also require a fresh review. Saved-only
and metadata-only records can be explicitly purged. The main cannot remove itself.

The existing journal stages/readbacks the candidate and the owner task reserves
capacity before membership revocation. Reset outcome is tracked separately from
applied configuration cleanup. Storage/metadata failures remain explicitly
incomplete. Direct Replace remains available without destructive Remove first,
preserving mapped equipment/activity/slot IDs while revoking and forgetting the
old isolated device. The requested device-card and replacement-dialog sentences
are removed; clean activity projections refresh while dirty drafts require reload.

Targeted native checks cover all-role cascades, unrelated values, slot metadata,
stage/reservation/marker failures, metadata storage/reboot behavior, saved-only
retry, exact reviewed sets and optional boot changes. Thirty browser checks and
three actual-script Node UI suites pass; all 317 Python framework checks pass.
The ESP-IDF build passes 36 public record layout checks. Logs are under
`test-results/device-removal-2026-10-29/`. These checks do not qualify physical
execution of the new removal behavior; deployment and functional evidence must
be recorded separately after running.

## 0.1.102 bench interruption and empty-context correction — 30 October 2026

All four DUTs were updated and verified on exact 0.1.102, image SHA-256
`1df92c63d9f2f3c56d224a4242e99b703597743c67a782a7a9b0f342623c918e`,
ELF SHA-256 `e6bfe91778c5efded3fa2f3f012ce093bf226d1d0bbfaa83f9854da2d265078b`,
firmware commit `2b6e9a604a018b319050f02751eb918dd847e4b7`. The complete host
runner passed, including 339 framework units and the new TLS/worker regressions.
The first physical attempt was interrupted before scan/activity execution by
simultaneous receive-RPC timeouts on all six Pi CEC channels. Preserve that
failed report; it does not qualify or disqualify the repaired DUT context path.
A separate 45.053-second six-wire idle probe passed 4,152 receive RPCs with
maximum RTT 188.615 ms under the unchanged 1.05-second deadline. It used only
open/receive/close, with no configure, transmit or DUT-control call. It did not
reproduce the transient bench interruption. Evidence is under
`test-results/consortium-context-fix-2026-10-30/`.

Cleanup exposed a separate empty-selector completion regression: a successful
current-context capture emitted no message, so no FIFO admission cleared the
refresh flag and reviewed Remove rejected the otherwise-ready added member.
The narrow correction clears refresh-pending on exact-current successful
completion without granting context readiness or repeating an empty capture.
Actual owner-task and runtime-removal before/after regressions pass. The later
0.1.103 deployment, Add pass and failed three-owner run are recorded above;
this 0.1.102 bench attempt remains failed.

## 0.1.101 connection failure reproduced — 30 October 2026

All four DUTs were updated and verified on exact 0.1.101 from firmware commit
`2413ce72259a405aee6620f218a3873cc2432d06`: image SHA-256
`9a3fef17bed1f1da3a84d731ba08c82864e6d98a454c98440ba81d366b9ed5c7`,
ELF SHA-256 `ff3a4e3b469b622889058e1d3e0a061842155f764931237430b388b9011d7a7f`.
The complete host suite passes with 339 framework units after a test-only
extractor update. The two-owner physical case passes with fourteen fresh power
assertions, six typed locations, empty owner failures, two theft responses at
66.900023/66.900044 ms and clean reset cleanup. Evidence is under
`test-results/consortium-link-trace-2026-10-30/`.

The three-owner run failed and remains failed. Its retained owner snapshot
shows member 2 still enrolled but unready with no current boot; all main,
member-3 and proof-object checks passed. The first inactive player's physical
announcement completed, but the owner check rejected that disconnected member
before the timed response assertion. Firmware tracing locates the disconnect
earlier, during the activity-save configuration refresh: the future-revision
context waited 1,150 ms under its original 1,000 ms lifetime and fenced the link.
Fresh reviewed removal later acknowledged both fixture resets and restored all
main saved values except normal revision counters, retaining the same main boot.

The actual-code native reproduction admits only that future nonemitting
context, delays the real synchronization exchange 1,150 ms and observes its
causal expiry. The correction uses the existing post-sync current-context
callback without retaining/replaying the old payload or extending action TTLs.
A separate actual-source regression proves that the old whole-frame TLS read
can prevent admitted work from advancing for 1,180 ms; incremental receive
preserves the original framing/operational deadlines while servicing IPC.
These corrections target 0.1.102, whose build, deployment and physical results
must be recorded separately. They do not retroactively prove DUT receipt of
the original 0.1.99 missing source-theft frame.

## 0.1.100 deployment and strict-deadline physical rerun — 30 October 2026

All four DUTs run the exact 2,428,224-byte 0.1.100 image from commit
`291b9239065a8265e137928c05227e7f41293a08` (`source_dirty=true`), ELF SHA-256
`a46975167832e538590835e8f830aeecfad4ed3b5b0c927f46a20249b359449d`.
Main OTA passed 13 preservation/health checks; each application-only candidate
update passed 18 checks with full private backup and protected-flash equality.
Evidence is under `test-results/consortium-runtime-fix-2026-10-29/`.
The complete host suite passed with 333 framework tests and 36 public layouts.

The two-owner consortium passed: six typed locations, fourteen fresh physical
power assertions, both theft directions at 66.900 ms, clean fixture removal and
acknowledged member reset. Independent report verification recomputes latency
from correlated same-component/wire TX, RX and applied-state records.
The separate shared-wire source-theft case also passed with clean cleanup:
directed Standby at 66.899909 ms and route restoration at 183.349990 ms within
the same original two-second window. Reports are `consortium.json`,
`consortium-verification.json` and `source-theft.json` in that directory.

The three-owner case failed. It proved nine typed locations and all three
owner boots, then completed the main activity, two theft responses at 69.949991
and 68.100104 ms, and all three fresh final Standby commands. Before submitting
the member-2 start, a fresh system owner-proof assertion failed. Its rejected
snapshot and assertion message were not retained. Initial member cleanup failed;
a later read showed both exact member boots ready. Fresh reviewed cleanup then
removed only the two known fixtures, acknowledged both resets and preserved all
main saved values except ordinary revisions and the same post-OTA boot.
`consortium-three-duts.json` remains failed; `manual-cleanup.json` records recovery.
No missing-receipt/source-policy cause is inferred from these later passes.

The next framework revision retains bounded sanitized owner-failure snapshots
and controlled primary assertion/cause stacks in both stdio and JSON; arbitrary
exception text and HTTP payloads remain excluded. All 339 framework units pass.
Firmware diagnostics add reason-tagged operational link closures without
changing admission, queue, lease or source policy. A separately reproduced
live-refresh bug now delivers already-admitted typed lookup results before
configuration advance; its host proof does not establish the cause of this
physical readiness failure. Target 0.1.101 still requires deployment and a new
physical qualification. Earlier unmatched/pre-review build artifacts were
caught by integrity assertions and were never deployed.

## Enrollment timing, CEC deadlines and 0.1.99 bench results — 29 October 2026

All four bench DUTs were updated and verified against the exact 0.1.99 image:
2,423,968 bytes, image SHA-256
`308cc76b072a995cc1077234af7b2d1b33a963a21eabadbb3f4ae1fae97491d4`,
ELF SHA-256
`e1e52d22fad8918f3803b1c7526ca47e3114e4e7759719b5098451fca9fc5787`.
Main used authenticated application OTA without USB access; the other three
used MAC-locked application-only writes with backups and protected-flash
comparisons. No full erase was used. `deployment.json` under
`test-results/device-removal-2026-10-29/` passes and retains source commit
`636a4fc3ad4f490d3310651a76ab9fc16552eda8` with `source_dirty=true`.
This establishes deployment, not functional parity or source-theft timing.

Both physical consortium cases on that image failed. `consortium.json` failed
during scan verification, before creating activities; its initial cleanup also
failed and required separately recorded recovery. `consortium-three-duts.json`
proved all nine typed locations and three owner boots, then reached activity
and theft checks. While the member-2 activity was selected, the main's player
successfully transmitted Active Source but did not receive the required
directed Standby and remained On. The then-shared 180-second timeout expired.
The cause of the missing source-theft response, including whether the DUT
received that announcement, remains unresolved. Do not infer a received DUT
frame from the harness's successful physical transmission alone.

That primary power timeout was masked in the outer result by cleanup failures:
the unused innermost activity was initially closed while another fixture
activity was active, so its existing standby-only edit fence rejected removal.
The TestBed later retried the registered cleanups, removed all fixture
activities/equipment, and received reset `scheduled` for both members.
Framework context exits now preserve an already-propagating primary failure
when an early cleanup attempt fails, while retaining its diagnostic record and
the registered TestBed retry. A cleanup failure after a successful body still
fails the test; unresolved final cleanup still fails and chains the original
cause. Host regressions exercise the actual standby fence and those retry paths.
`main-final-postcheck.json` remains failed because the functional coverage did
not pass. Its preservation checks pass: the main retains its post-OTA boot,
authority, group, Matter fabrics, HomeKit state and capabilities, with no active
peers, removed owner names/observations or unexpected saved-value changes.
The authorized full removal of saved owner-2/3 records and normal fixture
tombstones are the only allowed configuration changes. Earlier failed reports
and `manual-cleanup/cleanup.json` are retained separately.

A separate unchanged normal-profile Add-only run, without CEC component
traffic, passed on the original main and `amity-dut-2`. It measured 95.494
seconds from helper entry to verified ready, 59.272 seconds from Add POST to
membership-operation completion, and 67.940 seconds from Add POST to verified
settings/ready. The measurement includes host HTTP timing; cleanup acknowledged
reset `scheduled`. Evidence is `enrollment-timing.json`, `enrollment-timing.log`
and `enrollment-measurement.json` in the same directory. This single enrollment
measurement does not set an acceptable CEC remediation delay.

The framework now separates the operation budget (`--timeout`, default 180
seconds, accommodating the measured 95.494-second Add) from ordinary fresh CEC
power/routing waits (`--cec-timeout`, default
five seconds). Source-theft remediation has an independent hard two-second
window: complete directed Standby and resulting state after that component's
complete physical Active Source transmission, plus complete Set Stream Path
restoration on the same original window in the shared-wire case. Isolated
consortium wires retain their existing absence of a route-restoration command
requirement. The maximum is two seconds; normal remediation should be much
faster.

The deadline uses correlated same-session/same-wire physical TX/RX timestamps.
[Linux CEC defines those timestamps at the last transmitted/received byte](https://www.kernel.org/doc/html/v6.19/userspace-api/media/cec/cec-ioc-receive.html).
The Pi mapping applies the same constant offset to both events, so subtracting
them preserves that wire interval. Timely history can be validated after a slow
owner-boot HTTP proof, but a late frame cannot pass or restart the deadline.
Reports retain the measured response latency. The deadline checkpoint passed
328 Python framework tests, including late-history rejection, successful
delayed owner proof, exact
trigger provenance, the shared routing deadline and independent timeout
settings. After the five cleanup-preservation regressions were added, the full
Python framework suite passed 333 tests. These stricter bounds were added after
the failed 0.1.99 runs and
have not yet been physically qualified. Do not relabel the historical failures
or the older two-owner success as a pass for this stricter timing contract.

## Three-Amity physical attempts — 29 October 2026

The new `test_consortium_three_duts` case was run on `test2` with `amity-dut`
as the main and `amity-dut-2`/`amity-dut-3` as candidates. The main reported
firmware 0.1.98. A frozen copy of the framework was verified identical locally
and on the Pi; no firmware or assertion changes were made for either attempt.

The first attempt exhausted its default 90-second Add budget during the first
member's settings verification, ending with an HTTPS handshake timeout. Only
member 2 was added, then removed with reset `scheduled` and no cleanup errors.
The fresh postcheck preserved every saved value except revision counters, along
with the main's boot, authority, Matter fabrics and HomeKit state. The user
reported accessing the UI during this attempt; its contribution to the delay
is unproven. Preserve this failed report at
`test-results/consortium-three-duts-2026-10-29/functional.json` and its stdio log.

The unchanged retry used the existing `--timeout 180` option. Both members
passed exact connected/configuration/settings checks. During the HDMI scan,
`GET /api/system` failed with `IncompleteRead`: 4,096 bytes arrived from a
6,218-byte response. The retained scan snapshot contained nine devices, six
wire initiators and three owner boots, but the scan did not return and none of
its explicit membership/location assertions or activity/theft checks ran.
Both newly added members were removed in reverse order with reset `scheduled`,
no retained membership and no cleanup errors. The main remained ready/idle with
the same boot, authority, activities, equipment, sixteen reserved slots, Matter
fabrics and HomeKit state. Its only saved-value change was the retained device
record for member 3; the postcheck records that difference rather than declaring
exact configuration equality. No active peers remained.

Retry evidence is under `test-results/consortium-three-duts-retry-2026-10-29/`:
`functional.json`, `functional-run.log`, `main-pre.json`, `main-post.json`,
`provenance.json` and `physical-summary.json`. Neither attempt qualifies
three-owner CEC activity or source-theft behavior.

## Current saved format only — 29 October 2026

Firmware 0.1.96/Matter software 97 removes older saved-format readers, migration
backups/maps and imports, and versioned project names/routes. Only current
configuration schema 3 and storage format 2 are read or written. Current valid
configuration/journal bytes, mandatory original authority, independent
factory/origin/settings formats and the original CEC/activity/input algorithms
are preserved. Numeric editor document version 1 and system API version 2 remain
current protocol metadata. Unsupported saved data is rejected without migration.

The native build passes all 35 layout checks; the full host runner exits 0 with
259 framework units. Browser checks pass: 90 activity, 27 system, 14 member-update
and 21 device-settings checks. Regressions cover the activity application's
incorrect retry after a removed-route 404 and the system view's raw-configuration
collision with the activity editor document. Offline ASan/UBSan comparisons prove
current serialization matches the parent implementation, including the captured
main configuration. Final evidence is under
`test-results/current-format-final-2026-10-29/`: `build.log`, `host-tests.log` and
`firmware-0.1.96/`, plus `browser-activities.json`, `browser-system.json` and
`browser-device-settings.json`. All 14 final member-update checks also pass.
The frozen app is 2,401,632 bytes, SHA-256
`27f600378f0e663a64de79104516e90b47f32083b6798d8cf0e08b8abd69ed02`,
ELF `e3fd9fe9b3e6be1e1b4ad46dcf7d72516de3383c06bc7afe1e2d025b40b29622`,
built from parent `bf973d7` with `source_dirty=true`.

The intermediate 0.1.95 image deployed successfully on all four DUTs. Its
functional attempt was interrupted for the final browser cleanup after scan and
activity passed; it does not qualify the complete suite. Preserve
`functional-interrupted.json` and the failed `interrupted-cleanup.json` in
`test-results/current-format-cleanup-2026-10-29/`. Separate
`interrupted-recovery-verification.json` passes equipment recovery with four new
reserved slots retained as retired entries, giving a thirteen-slot baseline.
All four final 0.1.96 deployments pass: main authenticated OTA passes 13
preservation checks, and each application-only USB update passes 18. The new
main boot is `5542bba1eda203612d8e5e5b0af2c95c`; configuration, two Matter
fabrics, HomeKit, original authority and capabilities are identical through OTA.
Updates use no main USB access, factory reset or full-flash erase. USB non-application
and OTA equality is checked before normal boot writes. Evidence is
`test-results/current-format-final-2026-10-29/deployment.json` and its four
device reports. The complete `functional.json` now passes all five scenarios:
HDMI scan, activity power, explicit switch input, source theft and Add Amity.
Every testbed passes without cleanup errors. Add confirms candidate
`7468ba8afeb9ffad49eb57b3ba0a3288`, two connected/settings assertions on one
member boot, completed removal with reset `scheduled` and no retained member.
Final `main-final-postcheck.json` passes all 23 checks and logs out, retaining
the same post-OTA boot, authority/group, settings/capabilities, two Matter fabrics,
HomeKit and all saved values. All thirteen baseline slots remain identical; only
ordinary revisions and three exact fixture-owned retired slots are added. The
fourth created activity is valid without an external slot because the unchanged
sixteen-slot registry is full. Configuration/presentation revisions advance 42→54,
state 1→19 and membership 10→12; whole-document equality after tests is false as
expected. This qualifies the exercised local GPIO CEC paths with simulated
equipment and the normal two-board Add/removal path, not strict-profile admission,
distributed CEC actuation, real AV behavior or all-board interoperability.

## Audit fixes — 28 October 2026

Firmware `0.1.88` fixes four audited cases: confirmed radio cleanup after a timeout
returns to ordinary operation without reviving expired setup; a rejected local
key-down leaves no token that can swallow a press after adapter return; Add and
reset use one accepted physical press-edge clock; scan evidence validates the
actual OSD-name and physical-address/device-type reply bytes. The owning activity,
membership, radio and functional-test documents record those contracts.

`sh tests/run_host_tests.sh` exited 0, including the new radio-worker, qualified
local-key/reconnect and GPIO gesture regressions and all 165 harness unit tests.
The log is `test-results/audit-fixes-2026-10-28/host-tests.log`. Independent focused
CEC/owner, consortium, radio and harness suites also passed. The new scan negatives
reject matching cached UI state paired with rewritten identity replies.

The pinned SDK `idf.py build provisioning-package` passed for the existing
four-adapter profile, including all 35 public-record layout checks. This build
also includes the separately pending reference and 12 KiB task-stack changes;
its image hashes describe that combined working tree. The image is
2,418,752 bytes (`0x24e840`), leaving 39% of its OTA slot free; its SHA-256 is
`3054b98afccb451c2926e29088e7297775a8293b915f82ae1a9319b4161e5688` and ELF identity is
`3c17f9bfc32283ea4812501040bf9091fd360c9ae8b1be0238524f8695aa6cc8`.
Build evidence is `test-results/audit-fixes-2026-10-28/build.log`. This image has not
been deployed, and the strengthened scan assertions have not run on the physical
bench. Earlier physical results remain separate evidence.

## amity-dut update and reset — 29 October 2026

The subsequent build from committed source `2d148ca` passed with the pinned SDK
and was installed on `amity-dut` through the existing authenticated firmware
endpoint. Version `0.1.88` and ELF identity
`c9849838205bbf60f2558603f44d035d896a33f4b714bf062bb17c4878b48c2e` booted
successfully. The saved `0.1.86` configuration still contained the removed empty
`quirks` field, which the new reader rejects; the initial boot therefore retained
storage with activity control passive. A healthy image alone did not establish
successful configuration startup.

The user authorized a factory reset. The existing harness helper parsed the
documented reset wiring, held only this DUT's FW RESET for 10.2 seconds, and
released it to input with pulls disabled. The user then confirmed Matter
re-enrollment. Authenticated verification confirmed the same stable device ID,
the expected version/ELF, ready original-main authority and UI, working factory
initial password, two Matter fabrics and an empty new configuration. Boot ID is
`85adbb9ed72b69033230ba8784237cb0`. Factory credentials were retained; old
configuration and pairings were explicitly reset. The user restored hostname
`amity-dut`. Read-only checks after that restart verified boot
`adf4abfb5b747b0b0cf5ef5aa03e393f`, the same expected image, ready main/UI,
unchanged fresh configuration, working factory password, two fabrics and a closed
commissioning window.

Private evidence is in `test-results/audit-deployment-0.1.88/`, including
`build.log`, `deployment.json`, `new-boot-diagnostics.json`,
`factory-reset-action.json`, `factory-reset-uart.log`, `after-reset.json` and
`after-hostname.json`.
No other DUT was updated in that operation. This verifies image, reset and main/UI readiness;
physical CEC/AV/key behavior, multi-adapter timing and Amity admission remain
separate qualification work.

Current credential records are consolidated in the ignored private
`test-results/credentials/` directory. Cleanup removed 89 older or duplicate
credential carriers and redacted superseded credentials in 13 historical
evidence files. The four current records and latest original provisioner cards
were preserved; no setup-card QR, password image or instructions were changed.

## Remaining DUT updates — 29 October 2026

`amity-dut-2`, `amity-dut-3` and `amity-dut-4` were updated by USB from 0.1.84
to the same exact 0.1.88 image above (Matter software 89, source `2d148ca`,
release metadata `source_dirty: true`). All three per-board reports passed:
exact written image and running ELF, retained factory credentials and origin IDs,
ready unclaimed startup, automatic Matter setup/window/advertising and the SDK
admission self-test. Non-application flash and OTA metadata were unchanged before
restart; no full-flash erase or factory reset was performed, and setup cards were
unchanged. The already-current `amity-dut` was skipped without opening USB or
restarting it; authenticated read-only checks confirmed its same boot, original
main authority, unchanged configuration and two Matter fabrics. The aggregate
report is `test-results/firmware-update-2026-10-29/deployment.json`, with per-board
reports and main pre/post checks in that directory. This verifies image and
startup state, not a new Add/Remove attempt, a fix for the observed `fw_reset`
overflow, CEC/AV behavior or radio admission. The failed Add evidence below remains
unchanged.

## Add Amity functional test — 29 October 2026

The new `add_amity.py` uses `amity-dut` and the documented `amity-dut-2`,
skipping candidate HTTP while controlling its parsed FW RESET wiring. All 238
framework unit tests pass. Physical qualification fails: the initial discovery
attempt hit a TLS handshake timeout; a retry matching the UI's admission-only
polling stayed responsive but never discovered the candidate. Firmware 0.1.84
on the candidate logged a stack overflow in `fw_reset` and a software reboot
during the attempt. No Add or Remove was submitted, and the scans were canceled.
The connected-member and scheduled-reset assertions have not passed on hardware.

Opening diagnostic serial ports also caused unintended USB serial resets on
both boards; capture was stopped. The original main's exact saved configuration,
controller identity and authority were subsequently verified, and its UI reports
two current Matter fabrics. Reports are under `test-results/functional/`:
`add-amity-2026-10-29.json`, `add-amity-retry-2026-10-29.json`,
`add-amity-retry-amity-dut-2-uart.log`, `add-amity-after-2026-10-29.json` and
`add-amity-main-matter-after.json`. The first failure is retained as a failure;
no blanket request retry, weaker assertion, new firmware API or firmware change
was introduced to turn either result into a pass.

## Add discovery rerun and pending stack fix — 29 October 2026

After all four boards were verified on 0.1.88, `add_amity.py` still found no
candidate: 40 admission polls returned `scanning` before the 90-second deadline.
The final short-deadline request surfaced a TLS handshake timeout; this is not
evidence of sustained HTTPS failure. No Add or Remove was submitted. Cancel
completed, the parsed GPIO was released to input with pulls disabled, and
postchecks confirmed the main's same boot, exact configuration, authority and
two Matter fabrics. Evidence is
`test-results/functional/add-amity-latest-0.1.88-2026-10-29.json` and its
`-before.json`, `-after.json` and `-pin-after.json` companions. This run did not
capture a candidate crash or establish why it was absent.

Target compiler analysis identified a concrete stack defect consistent with the
earlier 0.1.84 UART overflow: the synchronous Add handoff closes the Matter window
and refreshes DNS-SD, with a reachable 3,408-byte subtotal on a 3,072-byte task.
The temporary split probe's manual-Matter chain totals 3,792 bytes; the current
source with diagnostics totals 3,808, and publishing adds another 560 before
further allocator/logging/RTOS overhead. These are incomplete static subtotals,
not measured runtime stack margins. Private compiler records are in
`test-results/add-amity-debug-2026-10-29/stack-analysis/`.

Prepared firmware 0.1.89 (Matter software 90) separates manual Matter setup into
a `noinline` helper, reducing the actual Add callback frame from 1,456 to 160
bytes, and increases `fw_reset` from 3,072 to 6,144 bytes: 3,072 additional
internal-RAM bytes once. It adds reservation/window failure and post-callback
stack-margin diagnostics. Gesture thresholds, callback/task ownership, radio
budgets and call order are preserved. Focused startup/automatic-Matter checks,
22 interrupt scenarios and 10 radio-handoff cases pass. The final native build
and all 35 public-record layout checks pass; its log is
`test-results/add-amity-debug-2026-10-29/build-final-0.1.89.log`. At that stage
this source had not been deployed or physically qualified; the verified deployment was 0.1.88.
The failed Add runs remain failures. Later gesture and debug evidence follows;
connected-member and acknowledged removal/reset qualification remains separate.

## Add debugging and continued Remove failure — 29 October 2026

The candidate's exact 0.1.89 image was deployed and its actual 3.5-second setup
gesture opened Add with 3,968 bytes of minimum free `fw_reset` stack. The
functional attempt still failed. This establishes an exercised gesture/stack
margin, not a completed Add/Remove result or a bound on every SDK stack peak.
Evidence includes `44b176aec5e0/deployment.json`, `functional-uart.log` in that
subdirectory, and `add-amity-main90-instrumented.json` under
`test-results/add-amity-debug-2026-10-29/`.

Further diagnosis isolated four additional paths. Main 0.1.90 negotiates ATT
MTU explicitly before GATT service/record exchange; the previous default MTU
fragmented the real proposal into 195 ten-byte payloads and exceeded the
unchanged 30-second assembly deadline. Candidate 0.1.91 waits for usable IPv4
within the original single 35-second network budget: the SDK success callback
arrived at association before DHCP, so the prior immediate IP test rolled back
a connection that was still progressing.

The framework's connected-member assertion now polls the shipping device-settings
endpoint directly while its two-second response cache is live. It then repeats
main boot/authority, existing saved activity/equipment/settings records,
membership/name/capacity and member-boot checks. All 240 framework unit tests pass; no assertion, cache TTL,
operation deadline or firmware API was weakened or extended.

The subsequent Remove failure exposed an in-flight settings reply before its
retirement acknowledgment. Main 0.1.92 admits that envelope only for discard
under the same management capability/TTL/replay and exact peer/group/boot/
epoch/configuration checks, immediately restores lifecycle restriction, and
keeps the existing deadline and 32-frame receive bound. Nothing in the discarded
reply is applied. The separately authorized candidate reset restored clean
unclaimed startup and automatic Matter setup after the failed removal; those
recovery checks do not qualify the functional Remove/reset acknowledgment.

The final 0.1.92 native build passes all 35 public-record layout checks, and
`sh tests/run_host_tests.sh` exits 0 including all 240 framework unit tests.
Logs are `build-0.1.92.log` and `full-host-tests.log` in the debug directory.
The 2,419,808-byte image SHA-256 is
`0bd2bc66d7488314377b13de95fe80b6405e58bd979f3b1e6166f157c368cc4e`, with ELF
identity `8c6dee1f0960ad0480179f341e2181cfa95de1414e2eb9fc1253ea2bea497461`.

Main OTA to the exact final 0.1.92 image passed all 13 deployment/preservation
checks without opening main USB. It retained the original authority, full
saved configuration, two Matter fabrics, HomeKit pairing and hardware
capabilities; the OTA caused the expected new boot. The report is
`main-0.1.92-deployment.json`, with `main-0.1.92-before-update.json` and
`main-0.1.92-after-update.json` in the debug directory. Candidate 0.1.91 was
recovered to clean unclaimed startup before another attempt.

The fresh unchanged `add_amity.py` test on main 0.1.92/candidate 0.1.91 completes
Add and passes the exact connected-member/settings assertion twice. Remove
reaches operation `complete` but reset `unconfirmed`, so cleanup raises
`MembershipCleanupError` and the overall test remains FAIL. Candidate UART
records `esp_task_stack_is_sane_cache_disabled()` during flash handling, then
a later `xQueueSemaphoreTake` assertion; unclaimed startup and automatic Matter
setup after removal were not confirmed. The report is
`add-amity-main-0.1.92.json`, with `add-amity-main92-instrumented.json` and
`candidate-during-main92-test.log` in the same directory. End-of-run main
preservation checks were separate from that failed cleanup. The subsequent
source/instruction analysis below identifies the two candidate paths; the
original failed result is retained.

Intermediate failed reports remain failures, including `add-amity-0.1.89.json`,
`add-amity-main-0.1.90.json` and `original-add-recovery.json`. No new firmware API,
weaker reset acknowledgment criterion or CEC policy is introduced.

## Candidate restart/lifetime fixes and repeated physical PASS

Selected unchanged instruction windows from the 0.1.91 application, compared
with the archived symbol-bearing 0.1.92 ELF, corroborate the shutdown path:
`esp_restart` invoked the diagnostics `checkpoint_hours` NVS write from the
PSRAM-backed `group_link` task. The other captured assertion maps to the main
lifecycle callback removing administration mDNS from a headless board with no
responder. This is selected-frame/source corroboration, not full symbolication
with the unavailable original 0.1.91 ELF. The stronger unique 24-byte-window mapping is
`candidate91-unique-byte-mapping.json`; the earlier selected-frame record remains
`candidate91-panic-analysis.json` in the debug directory.

Candidate 0.1.93 moves the same prepared-member restart through the existing
internal-flash-stack broker after the same acknowledgment and 500 ms delay.
Administration mDNS registration/refresh/removal is serialized; removal is
limited to an actually registered owned service and happens before Matter can
free the shared responder. It changes no activity, button or BLE algorithm.
The native build and `final-host-tests.log` pass, including all 240 framework
unit tests. Its application SHA-256 is
`b76e365410f45bef9282f9ebaad94822b926a384dba6ce02e5a8c6ae639259eb`, ELF
`c7dccbb80d8c7cd4d795df53e2f52de0979f074701c33bccbac43878be674bc8`.

The candidate's app-only 0.1.93 deployment passed all 16 checks, retaining factory
credentials/origin and protected non-application flash. The separately authorized
reset after the earlier failed Remove restored ready unclaimed startup and
automatic Matter setup without a panic (`candidate93-removal-reset-cleanup.json`).
That recovery remains separate from a passing functional removal acknowledgment.

Main 0.1.94 also latches the existing Add operation's completion in its transient
view after exact ready-peer/hostname proof. A later Remove cannot turn that
completed Add back into `finishing`; a new scan replaces the view. This is
operation reporting, not an availability permission, persistent journal or new
firmware API. Native 0.1.94 passes all 35 public-record layout checks. Its image
SHA-256 is `4baf834b5227114602811392427669ab5e1a7977b5d4b7f260b0c75ba548be4b`,
ELF `af74c59554e7d9dce73e0888ce1767d9bd1a2b8f26b24598cc6695da867655d2`.
The exact main OTA passed all 13 preservation checks without main USB access;
`main-0.1.94-deployment.json` and its before/after snapshots retain the original
main's configuration, two Matter fabrics, authority, HomeKit and capabilities.

The final `verified-host-tests.log` exits 0, including all 240 framework unit
tests. Both consecutive unchanged original `add_amity.py` runs on main 0.1.94/
candidate 0.1.93 pass. Each completes Add, proves the same member boot in both
connected/settings assertions, and completes cleanup once with reset `scheduled`,
no retained member and no cleanup errors. Candidate UART confirms unclaimed Ready
and automatic Matter setup after both resets, with no observed panic or Backtrace;
minimum free setup-task stack is 3,968 then 3,976 bytes. No main reboot or USB
access occurs between the runs. Reports are `add-amity-main-0.1.94.json`,
`add-amity-repeat-main94-candidate93.json` and `add-amity-main94-instrumented.json`.

Final authenticated HTTP checks in `add-amity-main-0.1.94-after.json` pass all
11 checks: same main boot `06be5911aab8027ffc5a806b20b5f48e`, group/identity and
original authority, exact saved configuration, unchanged two Matter fabrics,
ready idle state and no added candidate. Saved activities/equipment are empty
in this physical baseline and remain unchanged. The session logs out. At this
functional-run checkpoint, `amity-dut-3`/`amity-dut-4` were still on 0.1.88. This qualifies the exercised
two-board normal-profile Add/connected/settings/acknowledged-removal-reset path,
not CEC/AV behavior, the strict setup-code profile or all-board interoperability.
No API, assertion, transaction deadline or CEC policy is weakened. Prior
failures, including the 0.1.92/0.1.91 Remove failure, remain failures.

## All four DUTs on 0.1.94 — 29 October 2026

The later application-only USB updates of `amity-dut-2`, `amity-dut-3` and
`amity-dut-4` each pass all 18 checks. All four boards now run exact firmware
0.1.94/Matter software 95, application SHA-256
`4baf834b5227114602811392427669ab5e1a7977b5d4b7f260b0c75ba548be4b` and
ELF `af74c59554e7d9dce73e0888ce1767d9bd1a2b8f26b24598cc6695da867655d2`.
Factory credentials, origin identities, partition layout, non-application flash
and OTA metadata were preserved; flash-range equality was checked before reboot,
not after normal boot writes. No full-flash erase or factory reset was used.
Each updated board boots unclaimed Ready with automatic Matter setup and the
SDK admission self-test passing, without observed panic. Setup cards are unchanged.

The already-current main was skipped. Its authenticated read-only preflight
passes 12 checks and final postcheck passes 14, retaining the same boot, original
authority, exact configuration, two Matter fabrics, capabilities and empty peer
list. No main USB access, restart or mutation occurs; the HTTP session logs out.
Aggregate and per-board evidence is in
`test-results/all-dut-update-2026-10-29/deployment.json`, with
`main-preflight.json` and `main-postcheck.json` in that directory.

This is the existing tested artifact built from source commit `12b39e1` with
`source_dirty=true`; its fixes were subsequently committed as `a2987b8`. Artifact
provenance is retained. These updates qualify image/startup and preservation;
the earlier two Add/removal passes remain main-0.1.94/candidate-0.1.93 evidence.
The functional test was not rerun on 0.1.94/0.1.94, and no new CEC/AV,
strict-profile or all-board interoperability result is claimed.

## Activity switch-input sub-test — 29 October 2026

`--test activity` now runs independent power-cycle and switch-input sub-tests
from `cec-harness/functional_tests/activity.py`, each with its own scan,
temporary activity and cleanup. The added case uses the scanned AVR as audio
and switch, no CEC source, and requested input 4. This exercises the original
`Hub::fallback_input` behavior: a discovered CEC source uses Set Stream Path;
an absent source permits explicit Select AV Input Function. Firmware algorithms
and endpoints are unchanged.

The case requires the selected activity to be active, fresh physical power-on
commands for TV/AVR, the unselected player in Standby, and an applied directed
`44:69:04` from the scan-proven DUT initiator after activity start. Numeric input
state is separate from physical-address routing. Virtual/stale/wrong-address
traffic, modified/dropped hooks, ignored commands and local setters cannot prove
the input selection. All 259 framework unit tests pass, covering these evidence
boundaries, switch configuration/cleanup and independent sub-test reporting.

Both activity sub-tests pass on `test2` against `amity-dut` 0.1.94. The added
case observes wire frame `05:44:69:04` (DUT 0 to AVR 5), applies numeric input 4
and verifies the selected UI activity without a reboot. Both cases remove their
temporary activities/equipment with no cleanup errors. Private evidence is
`test-results/activity-switch-2026-10-29/functional.json`.
Final authenticated checks in `main-after.json` pass all 19 checks: same boot,
authority, saved activities/equipment/settings, capabilities and two Matter
fabrics, with the main idle and no peers or cleanup errors. Only revision
metadata and two normal reserved external-slot tombstones for the exact removed
test activities changed; preexisting slots and all other configuration values
are identical. The session logs out, with no main USB access or reboot.

## Source theft functional test — 29 October 2026

The new `source_theft.py` configures a saved activity for each of two playback
devices behind one AVR, starts the first, and makes the second claim Active
Source. All three functional scenarios pass on `test2` against `amity-dut`
0.1.88; all 204 framework unit tests also pass. The second playback model uses
the tuner allocation pool, retaining playback identity and behavior, so the
shared back wire can claim 5/4/3 in CEC 1.4 mode. The test observes actual DUT
directed Standby and Set Stream Path frames, verifies the original activity
and boot, and removes both temporary activities. Saved activities/equipment
are preserved and all 14 GPIO address masks are clear after cleanup. Reports
are `test-results/functional/source-theft-suite-0.1.88.json` and
`source-theft-verified.json`; host output is `source-theft-framework-tests.log`
in the same directory. No firmware API or behavior changes were made. This
qualifies the exercised local CEC path with simulated components, not real AV
behavior, all adapters or remote radio delivery.

## Standby stack fix — 28 October 2026

The activity functional test committed at `c4833a1` exposed a firmware failure on `amity-dut` running 0.1.85: start completed and the TV, AVR and player each received a real DUT power-on command, then standby caused an `amity_hub` task stack overflow and software CPU reset. The original failure report is `test-results/functional/activity-suite-0.1.85.json`; the detected overflow is in `test-results/functional/activity-dut-uart.log`. Target-compiled stack analysis found a standby call-chain subtotal above 6,304 bytes before downstream owner-send/runtime overhead, exceeding the 6,144-byte allocation. No recursive callback cycle was found.

Firmware 0.1.86 increases only that shared task stack from 6,144 to 12,288 bytes. The additional internal-RAM allocation is 6,144 bytes once, including on the unchanged four-tentacle profile. Activity planning, CEC handling, timing, attempt budgets and owner delivery are unchanged. Both `sh tests/cec_hub_test.sh` and `sh tests/owned_hub_test.sh` passed; logs are `test-results/functional/standby-stack-cec-host.log` and `test-results/functional/standby-stack-owned-host.log`. The pinned SDK build passed in `test-results/functional/standby-stack-build-0.1.86.log`, including all 35 public-record layout checks. The application uses `0x2528f0` bytes of its `0x3d0000` OTA slot, leaving 39% free.

Only `amity-dut` was updated to 0.1.86 through the existing authenticated application-update route. Its four-tentacle profile and existing configuration were preserved. The verified running ELF SHA-256 is `6cd6bb5195da40c2174aefc7d5b86993b5966cd556fc4e8f3a96844c34a32997`, with healthy boot ID `0c508459411bbdbf06b380cf41f7f46b`. The update evidence is `test-results/standby-stack-fix-0.1.86/after-update.json`. The earlier 0.1.84 four-board Matter boot/setup evidence remains in `IMPLEMENTATION_STATUS.json` and its original artifacts; this update makes no new deployment claim for the other three boards.

The complete HDMI scan/activity suite passed in `test-results/standby-stack-fix-0.1.86/functional-suite.json`. Two further activity cycles passed in `activity-cycle-2.json` and `activity-cycle-3.json` in the same directory. All three cycles kept the verified boot identity, observed real DUT-origin power-on commands and directed Standby `0x36` frames to all three simulated components, completed UI operations/settling, and removed only the temporary activity and its unreferenced fixture equipment. Final `verified.json` confirms the original activities and equipment were preserved; expected revisions and normal external-slot tombstones advanced. `adapters-after.json` confirms all 14 harness Linux CEC adapters released their claimed logical addresses after cleanup. These tests use existing authenticated UI endpoints and actual GPIO CEC traffic; virtual component traffic cannot satisfy the physical power assertions.

Current internal free heap was 32,283 bytes after update, 32,031 bytes after the first cycle and 32,015 bytes after all three cycles; the largest internal free block was 11,264 bytes at all three snapshots. See `after-update.json`, `after-cycle-1.json` and `verified.json` in the same directory. Remaining task-stack margin and post-workload historical minimum heap were not measured. Host workers run on the host stack, so their passing tests do not establish target stack capacity. The physical regression evidence is completion of the exercised start/standby paths, actual standby frames and an unchanged boot identity. Four-adapter/eight-wire electrical timing, ESP32 owner-to-owner radio delivery, real remote input and real AV behavior remain qualification gates.

## Direct admission revision — 27 October 2026

The current contracts pass with `.cache/contracts-python/bin/python`: 34 manifest files, 6 schemas, 17 positive and 59 negative vectors, 4 configured selectors, 19 current wire kinds and 119 local links. Retired relay kinds are rejected.

The full host runner exited 0 in `test-results/admission-2026-10-27/host-tests.log`, including 29 independent passive-harness tests. Updated harness checks use normal ephemeral encrypted admission, fragmented GATT confirmation, exact scan/token selection and retained TLS/removal cases; no separate Add code is produced. Actual API and UI suites also passed (`admission-api-tests.log` and `admission-ui-tests.log` in that directory). The normal native build passed in `build-normal.log`; final strict-build and focused crypto outcomes are tracked in `IMPLEMENTATION_STATUS.json` after execution.

No actual ESP32 radio admission or new physical deployment is claimed. The firmware SRP self-test is compiled but has not run on an ESP32 in this task. The earlier refactor evidence below remains separate from this admission revision.

## Previous qualified-lookup refactor — 27 October 2026

The full validator passed using `.cache/contracts-python/bin/python`: 34 manifest files, 6 schemas, 17 positive and 55 negative vectors, 4 configured selectors, 23 current wire kinds and 119 local links. Hardware-capacity-only validation also passed for four adapters/eight CEC ports. These are contract/software checks; they perform no firmware/device operations. Firmware tests/build results are recorded separately after execution.

The actual synchronization integration suite `sh tests/system_sync_test.sh` also passed with ASan/UBSan: minimal identity/revision handshake and first-lease nonemitting context hook, failed snapshot IPC admission, derived control/realtime/cleanup authority with unchanged receiver auth/deadline, deferred owner-local lookup results bound to the exact authenticated link, actual source presence despite CEC admission failure, no fabricated lookup after IPC failure, setup completion/pending-reply snapshot metadata, exact boot/link disconnect, callback cleanup on failed initialization, sparse scans, saved adapter capacity retention, applied-revision status and real journal commit/failure independent of offline peers. Its SystemControl boundary is a spy; actual owner/facade CEC behavior is covered separately.

The full host runner `sh tests/run_host_tests.sh` exited 0 using pinned SDK Python 3.14 plus the local contracts site-packages. Its log is `test-results/host/intent-full-suite.log`; coverage includes the retained CEC/owner/configuration/remote/protocol/security/update suites, 27 local hardware-harness tests and offline production-security profile. After the final duplicate-name lookup simplification, `sh tests/cec_hub_test.sh` passed all five baseline suites and `sh tests/owned_hub_test.sh` passed its main and nonzero-settling suites. Those final checks cover baseline first OSD match/side isolation, coordinator-cache independence, two-owner AVR/switch, startup taps, observation IPC pressure and global last-completion settling.

For the earlier 0.1.81 refactor, `idf.py build provisioning-package` passed with the pinned SDK environment, including 35 public-record layout checks and package generation. The application is 2,420,848 bytes (`0x24f070`) in the `0x3d0000` OTA slot, with `0x180f90` free (39%). Image SHA-256 is `f7a28b2ff519eacc32e3a7ed3550eff2d39d9c27f7c21cc0cad6e54fe306e5af`; the log is `build/qualified-lookup-build.log`. The final build includes the baseline first-name lookup correction. This task has not deployed that image to a device.

## Historical evidence

The earlier graph-model schema/vector counts and firmware checks belong to their original commits/artifacts. The 0.1.80 deployment verified two image/startup results and independent credentials; its retained activity metadata concealed disabled AVR/switch behavior. It is not parity evidence for this model. Historical details remain in git and existing test-results artifacts instead of being retained as a second active design.

## Physical scope

Documentation and host tests do not establish physical qualification. The latest consortium result above covers main/member2 radio delivery and distributed activity/source policy on one adapter each with simulated HDMI-CEC equipment. Earlier local-only results retain their narrower scope. These results do not establish real AV/remote behavior, three-owner operation, four-adapter timing, complete ecosystem recovery or production security. Record actual device identity, image/source, configuration, input/observations and wire captures separately for each physical result.
