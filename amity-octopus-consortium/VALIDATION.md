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

## TLS connection reuse — 1 November 2026

TLS authentication and transport lifetime are separate from configuration
synchronization and execution-session recovery. Healthy peer acquisition is
idempotent; synchronization retries reuse TLS, authenticated boots and replay
history. Round IDs create fresh logical execution scopes, with exact old-scope
cleanup before the barrier and no ordinary work replay. Frame expiry rejects
and drains late bytes on the same transport without extending its acceptance
deadline. Available PSRAM owns all cJSON allocations at startup; heap-failure
observations record exact allocation requests without allocating in the hook.

All four DUTs run exact 0.1.113 / Matter software 114, ELF SHA-256
`84c567ee42e93a426e34a8066bbb4a87b49f003bb8f4079848eeea8913a37eaf`;
the 2,461,456-byte application SHA-256 is
`a7dd4cd7e55530a3239c32a8270d84df0a0c966df3460ff2b0bf155b2116845a`.
The main's authenticated OTA passed 13 preservation checks without opening USB;
each other DUT passed 18 application-only update checks with retained factory
provisioning and full backups. The full host suite, 361 framework units, native
build and contracts passed. Ten firmware artifacts and all 58 frozen harness
files were hash-verified, including the Pi copy. Qualified source and tests
remained unchanged through the physical run; final changes only record evidence.

All seven physical cases passed in one 553.989190-second run: scan 17.890657 s,
activity 32.771596 s, explicit switch input 34.187210 s, shared-wire theft
42.612407 s, Add/Remove 46.972495 s, two-owner 142.874243 s and three-owner
236.646128 s. Independent report checks passed 21 assertions for each consortium
case and 20 for shared-wire theft. Shared-wire Standby completed in 68.699982 ms
and route restoration in 186.699994 ms from the same physical Active Source;
all eight isolated consortium theft responses were at most 68.600027 ms. Every
response obeyed the original 250 ms window. Nine fixture activities were removed,
all four member removals acknowledged scheduled reset, and all seven testbeds
closed without cleanup errors. Final authenticated preservation passed 31 checks:
original main boot/authority, pairings, unrelated configuration and existing slot
metadata preserved, Ready/idle and no remaining peer, saved removed-owner data,
pending configuration or activity/hardware error. Evidence is under
`test-results/tls-connection-reuse-0.1.113-2026-11-01/`.

The final retained 24,576-byte main log has no allocation-failure record. This
bounded window does not prove absence throughout the run. Both final peer closes
coincide with acknowledged fixture removals; one retirement race is logged as
`heartbeat_send`. The earlier AES allocation's exact request remains unknown;
PSRAM allocation ownership supplies headroom without claiming that causal proof.

Earlier failed evidence is preserved: the 0.1.111 seven-case run passed five cases; shared-wire source-theft
wire responses met 250 ms but the assertion failed on the session inspection
lock, and the three-owner case encountered AES allocation/TLS failures and a
truncated HTTPS response. The 0.1.112 main-only diagnostic had different member
firmware and exposed a 512/582-byte frame progress expiry; it does not qualify
distributed power behavior or identify the earlier failed allocation's size.
The harness inspection fix retains the original 250 ms wire deadline and
rejects late/virtual evidence even after slow owner proof. All 361 framework
units pass. Both failed runs' newly added members were removed with scheduled
reset acknowledgments; original-main identity, pairing and unrelated data were
verified preserved. Evidence is under
`test-results/tls-connection-reuse-0.1.111-2026-11-01/` and
`test-results/tls-memory-diagnostic-0.1.112-2026-11-01/`.

## Current source-theft deadline — 1 November 2026

The functional tests now require thief Standby and resulting state within
250 ms of the component's successful physical Active Source transmission.
Shared-wire route restoration must finish within that same original window;
the two-owner and three-owner consortium cases require directed Standby on
the isolated thief wire. The Standby helper defaults to 250 ms; shared timing
validation rejects any larger requested bound, independently of operation and
ordinary CEC timeouts.
Boundary checks accept a complete frame at 250,000,000 ns and reject one at
250,000,001 ns, including a late frame already in history and slow owner proof.
The initial 355 framework checks passed with this limit; the extended 361-test
suite and 0.1.113 physical qualification above also pass. Earlier physical
results below retain their recorded limits.

## Final enrollment fixes and physical qualification — 30 October 2026

All four DUTs run exact 0.1.109 / Matter software 110 from
`6a94762a1e32c80230290d2f1b8f65b93696d1e1`, with the frozen release recording
`source_dirty=true` for the unrelated preexisting `hw/case` files. The
2,450,048-byte image has SHA-256
`ef397341d5e417f7d4a13141ed033830d8c6b9de005989a00c2669fb644818cf`;
ELF SHA-256 is `496867936d7fc6d8ffb5e90ba76b33f192620ba1e59d51d2df7964ca632862ac`.
Main authenticated OTA passed 13 preservation checks without opening main USB.
Each candidate passed 18 application-only update checks with full backups,
retained provisioning and closed USB before measurement. Ten firmware artifacts,
58 framework files locally/on the Pi and five asset references are hash-verified.
The complete host suite including 355 framework units, actual pinned-Mbed-TLS
security tests, boot/Matter tests and full contracts passed. Live web verification
passed 29 checks and all 11 response bodies completed. Evidence is under
`test-results/add-network-ready-fix-2026-10-30/`.

The fixes remove the minimum 20-second BLE discovery wait while retaining the
bounded collection window and later candidates; avoid repeated cursor writes at
an unchanged offset; remove operational TLS signature checks on exact already
validated credentials while preserving all untrusted ingress and mutual TLS;
reuse each DUT's authenticated HTTPS connection without redirects or resends;
remove headless admission tests and duplicate validation inside runtime startup;
and preserve the first known controller-address attempt until the station is up
with usable IPv4. Pre-IP waiting consumes no hint, mDNS query or retry backoff,
and still advances the existing 100 ms lifecycle/radio turns. Real post-IP
failures retain the original backoff, discovery and exact pinned authentication.

All seven physical cases passed in one 553.289206-second run, compared with
1,373.775839 seconds for the earlier complete 0.1.105 run:

| Case | Current duration |
| --- | --- |
| HDMI scan | 17.578577 s |
| Activity power cycle | 32.218851 s |
| Explicit switch input | 33.073205 s |
| Shared-wire source theft | 40.445718 s |
| Add Amity | 55.571156 s |
| Two-owner consortium | 145.136993 s |
| Three-owner consortium | 229.225704 s |

`functional.json`, faithful extracted cases and `functional-verification.json`
record seven clean testbeds, nine fixture activities created/deleted, four
completed Add transactions, four reviewed removals with acknowledged scheduled
resets and 28 typed scan locations. Independent two-owner/three-owner verifiers
pass 21 checks each; shared-wire theft passes 20. The consortium cases contain
38 fresh physical power assertions, 15 typed locations, five initial routes and
all eight cross-owner theft directions. Their slowest thief Standby was
69.450009 ms. Shared-wire thief Standby took 68.550109 ms and selected-route
restoration 186.550521 ms from the same successful physical Active Source
trigger. Every response met the unchanged one-second limit; selected activity,
owner boots and unaffected active routes remained correct.

The four full-run Adds compare the same test/candidate pairs with 0.1.105:

| Add | Earlier helper to verified | Current helper to verified | Current POST to observed completion |
| --- | --- | --- | --- |
| Standalone member 2 | 79.202 s | 50.056 s | 35.474 s |
| Two-owner member 2 | 79.083 s | 47.045 s | 33.655 s |
| Three-owner member 2 | 80.022 s | 41.928 s | 31.271 s |
| Three-owner member 3 | 84.557 s | 43.241 s | 33.621 s |

A separate complete Add-only phase diagnostic measured helper-to-verified time
44.578 seconds against clean 0.1.106 A2's 80.323 seconds; exact accepted-Add to
firmware-ready time was 34.380 seconds against 39.520 seconds. It retained both
clock anchors, exact image/boot proof, all preservation checks and acknowledged
reset cleanup. The main-clock partition accounts for every millisecond once:

| Disjoint wall interval | Duration |
| --- | --- |
| BLE connection/service setup | 0.996 s |
| Admission/identity/certificate/proposal/prepare | 6.866 s |
| Wi-Fi RPC through usable-network acknowledgment | 6.635 s |
| Both TLS setups and handshakes | 5.764 s |
| Hello/link/LAN proof/storage/hostname | 2.441 s |
| Final enrollment store to next TCP accept, including restart/rejoin | 6.881 s |
| Final configuration/owner synchronization | 1.688 s |
| Other unassigned transitions | 3.109 s |
| Total | 34.380 s |

Nested stages, the old prepared-link hostname timeout, callback sums and
concurrent HTTP times are not added again. These are elapsed wall measurements,
not CPU times or direct browser measurements. Wi-Fi RPC and restart/rejoin are
not subdivided without candidate-clock evidence, and unassigned transitions
remain unassigned. The combined firmware/framework comparison does not assign
its total gain to either alone. Samples are observations, not a latency
percentile or guaranteed bound; the 0.1.107 sample was faster than this sample.
`A1-phase-breakdown.json`, `A1-timing-comparison.json` and
`functional-add-timing-comparison.json` preserve clocks, hashes and these limits.

The final authenticated postcheck passes all 26 checks and logs out. Main boot
`5c10cc6c003ba549fe1ba5849ba4f252`, identity/group/original authority, exact saved
values, all 16 prior slot records, names, hostname, capabilities, Matter fabrics
and HomeKit pairing remain preserved apart from authorized revisions/cleanup.
The main is ready and idle with no peers, saved fixture equipment/activities,
removed-owner observations or pending configuration. Three main-owned discovery
observations remain independently cached as required. A historical membership
diagnostic remains; cumulative TX errors are 2,733 and RX drops zero. These
checks do not claim zero historical errors or diagnose that retained diagnostic.

The historical 0.1.106 A1 timing capture remains incomplete, and its extra
synchronization failure remains an unresolved historical cause. No additional
synchronization retry appears in the complete final Add-only phase trace; a
full-suite pass alone does not count internal reconnects. The first aggregate
summary attempt is also
preserved: a checker compared native tuple pairs with their JSON list form;
JSON-normalizing the recomputed dictionary fixes that representation mismatch
without changing any wire proof, assertion, deadline or saved result.

This qualifies the exercised normal admission profile, original main and up to
two added owners, one adapter per owner, isolated front/back wires and simulated
TV/AVR/player CEC responders. Strict physical admission, four-owner activity,
loss/adversarial/load testing, four-adapter electrical behavior and real picture/
audio hardware remain outside this evidence. All historical failed reports
retain their original outcomes.

## Headless startup delay fix — 30 October 2026

All four DUTs received exact 0.1.108 / Matter software 109 from
`044d912152a9d844bc221f4c29df587437da0395`: 2,449,632 bytes, image SHA-256
`7ad5b1404b8d3e98b07cc60af791f586b7ccf5ee07867c6a70d2ece3efea9af3`,
ELF SHA-256 `0bfe30379724e885c4bda7ef64a2f1940e9c73822268d93c844bc91f3e8a19e4`.
Main OTA passed 13 checks; each application-only candidate update passed 18
checks and closed USB. The full host suite with 355 framework units, contracts,
focused main/Matter boot tests and all 29 web checks passed. Evidence is under
`test-results/add-member-startup-fix-2026-10-30/`.

Validated prepared/member/retired startup no longer performs admission
self-tests, keeps admission `crypto_ready=false` and retains PSA/credential/TLS
validation. Unclaimed/original-main admission failure gates and explicit full USB
self-tests remain. Each `runtime_start` validates one group and identity rather
than validating them twice. Every public startup preflight still reads fresh
storage independently; there is no cross-call cache. Native before-fix tests
reproduce both duplicate validation and headless self-test blocking; all role,
profile, journal, authority, local-ID, remote-shutdown and fresh-read denials pass.

One normal-profile Add/removal diagnostic passed with both exact timing anchors,
zero discarded timing lines, all preservation checks and acknowledged reset.
Accepted Add to ready took 35.846 seconds, and helper to first verified ready
46.308 seconds. This single sample was slower than the 0.1.107 sample despite
less startup work: committed enrollment to next TCP accept took 10.927 seconds,
versus 7.942 seconds. Neither sample had the extra synchronization retry.
Do not claim an isolated or guaranteed latency gain for the removed startup work.

Source review then reproduced a reachable pre-IP connection path: committed
network recovery can return before usable IPv4, consuming the first numeric
hint on a failed socket, then waiting through existing backoff and a two-second
mDNS query. That mechanism is independently reproducible; main-only records do
not establish it as the cause of this particular 0.1.108 sample. The next change
addresses that path before full-suite qualification. No complete CEC suite was
run on 0.1.108; `A1-timing-comparison.json` preserves this timing observation.

## Enrollment latency fixes — 30 October 2026

All four DUTs received exact 0.1.107 / Matter software 108 from
`2fb1e895ef0581dea62d3b79fae1bb6393f597f4`: 2,449,008 bytes, image SHA-256
`2319eac138501a4be9c85a33742d261752f13d57c2597b4585cf3bea313c1684`,
ELF SHA-256 `e3183b6f42dcecbd7f2b6d333401622a1f6f798c309cdac2be70cebe38062aee`.
Main OTA passed 13 preservation checks, each candidate application-only update
passed 18 checks, and all USB ports closed before enrollment. The complete host
suite with 355 framework units, actual pinned-Mbed-TLS tests and full contracts
passed. Web verification passed 29 checks and all 11 complete bodies. The first
host run's missing native-fixture include is preserved in
`host-tests-first-attempt.log`; test-only commit `876be14` fixes it without
changing the frozen firmware or framework. Evidence is under
`test-results/add-phase-fix-2026-10-30/`.

The changes publish BLE candidates immediately while collecting later boards in
the same bounded scan, cancel/fence discovery before exact selected connection,
omit repeated cursor writes at an unchanged response offset and retain the
original ATT/reply deadlines. Operational TLS retains full signature validation
at all untrusted credential ingress plus exact DER/identity/SPKI binding and
mutual TLS; real native exchanges reduce repeated full validations from four to
zero. The framework reuses authenticated HTTPS on its one configured DUT origin,
with two-second idle expiry, complete bodies, bounded request deadlines,
cleanup closure and no redirects or automatic request resends.

One normal-profile Add-only physical diagnostic passed, including exact
identity/settings proof, acknowledged removal/reset and all preservation checks:

| Measured interval | Clean 0.1.106 A2 | 0.1.107 A1 |
| --- | --- | --- |
| Helper entry to first verified ready | 80.323 s | 39.923 s |
| Helper entry to Add POST | 28.770 s | 6.196 s |
| Accepted Add to firmware ready | 39.520 s | 31.194 s |
| POST to observed operation completion | 42.143 s | 31.471 s |
| Observed completion to full proof | 9.410 s | 2.256 s |

The helper duration decreased by 40.400 seconds in this observed pair. The
comparison covers the combined firmware/framework change; it cannot assign the
total improvement to either alone and is not a latency guarantee. Both exact
firmware-clock anchors survive with no rejected timing records. The new trace
includes a 6.434-second Wi-Fi RPC, a 7.942-second committed-enrollment-to-next-TCP
restart/rejoin interval and two TLS handshakes totaling 6.831 seconds. The
synchronization retry from 0.1.106 A1 did not recur in this run; its historical
cause remains unresolved. New terminal receive diagnostics distinguish parser,
SDK/EOF and wrapper failures if it recurs, while preserving all original limits.

`A1-timing-comparison.json` retains hashes, clocks, disjoint spans and cleanup.
This qualifies the single normal-profile Add/removal path only; a complete CEC
suite was not run on 0.1.107 before the further startup change. The 0.1.106
investigation below remains an immutable baseline, including its failed timing
capture.

## Enrollment phase investigation — 30 October 2026

All four DUTs received exact 0.1.106 / Matter software 107 from
`d150022f0752ac23968da65cf12b2f4644302f70`: image SHA-256
`9b192c3558f512579fc2768431d474685e6c77bd21af96ae7d358725168dbd75`,
ELF SHA-256 `4a67a4137ee2dc3d3bbc21639b4c1afad30270099f2440c41b994b4a3f7c6cbf`,
2,447,056 bytes. Main OTA passed 13 preservation checks; each candidate's
application-only update passed 18 checks, retained provisioning and closed USB.
The complete host suite, 340 framework units, focused timing/GATT tests and full
contracts passed. The initial firmware build failed strict indentation warnings;
`build-first-attempt.log` retains that failure before the committed formatting
fix and successful build. Evidence is under
`test-results/add-phase-timing-2026-10-30/`.

Three Add-only physical diagnostics use the unchanged shipping endpoints,
documented 3.5-second physical gesture and normal reviewed removal/reset.
All three functional reports passed, all resets were acknowledged `scheduled`,
and all main preservation checks passed. No USB monitor was opened during these
runs. The ordinary logs distinguish actual device monotonic stage times from
host HTTP observation and later identity/configuration/settings proof:

| Sample | Operation polling | Accepted Add to ready | Worker selection to ready | Timing result |
| --- | --- | --- | --- | --- |
| A1 | Original 0.4 s after response | Unavailable | 52.634 s | Incomplete: submission marker rolled out |
| B | 2 s after response, exact Add GET only | 37.688 s | 37.605 s | Complete |
| A2 | Original 0.4 s after response | 39.520 s | 39.518 s | Complete |

A1's functional pass does not repair its failed timing diagnostic. Its retained
24,576-byte log lacks the exact admission marker, so no accepted-Add total is
invented. It shows an extra failed post-reboot connection: configuration transfer
succeeded, then owner-state receive failed after 2.169 seconds; a subsequent TLS
connection and synchronization succeeded. The extra connection interval between
those two TCP accepts is 13.023 seconds. Existing logging does not distinguish
TLS EOF/error, frame rejection or frame-progress timeout. A2 succeeded with the
same standard polling, so these samples do not prove polling caused A1's failure.

The complete B trace accounts for every millisecond without adding overlapping
parents and children:

| Sequential wall interval | Duration |
| --- | --- |
| Dispatch to selected worker | 0.083 s |
| BLE connection, key exchange, identity/proposal preparation | 8.284 s |
| Wi-Fi credential RPC through usable-network acknowledgment | 6.485 s |
| First LAN connection, TLS, challenge and durable enrollment | 8.011 s |
| Committed enrollment to next TCP connection, including restart/rejoin | 8.694 s |
| Final TLS, Hello/hostname and configuration synchronization | 6.131 s |
| Total | 37.688 s |

The old link's 5.015-second hostname failure overlaps the restart/rejoin interval;
it is not an additional five seconds on this critical path. TLS setup alone
repeats full certificate checks, and its two main-side setup spans total 1.669
seconds; the two handshake spans total 7.982 seconds and include further
certificate checks, SDK work, scheduling and explicit waits. These are wall
intervals, not CPU attribution. Candidate association/DHCP and restart internals
are not separately measured by main-only logs.

A separate read-only six-request probe measured fresh HTTPS GETs at
1.056–1.429 seconds, versus 64.6–66.7 ms after the first request on one reused
connection. The framework currently opens a new TLS connection for every request,
including its seven final proof requests. This explains much of that roughly
nine-second proof cost; the probe approximates reuse and does not measure actual
browser Add latency. The fixed 20-second discovery collection gate precedes
these enrollment intervals. `add-phase-analysis.json` preserves all three
reports, their hashes, explicit missing evidence, overlaps and disjoint bins.

Timing logging itself has unmeasured overhead. The pinned USB-JTAG polling sink
can wait until 50 ms since its last successful write when the connected host does
not drain its FIFO, then drop bytes until progress resumes; formatting, ring-copy
and log-lock costs also remain. `observer-overhead.json` and
`scheduling-analysis.json` preserve exact source/config references. Do not treat
instrumented measurements as guaranteed production latency or infer CPU
starvation from task priority on a dual-core system. These diagnostics qualify
only the exercised normal-profile Add/removal paths. The complete CEC-suite
qualification below remains specific to 0.1.105.

## 0.1.105 complete functional suite and Add timing — 30 October 2026

All four DUTs run exact 0.1.105 / Matter software 106 from firmware commit
`33208f41923db9f88eec093940d5f096266fd47b`, with `source_dirty=true` as recorded
in the frozen release. The 2,437,744-byte image has SHA-256
`eeb7ff3f5674d8cd0a33d48d4fb0ce9cc6a5d3a2e938e81cd68c1ae4d1acc8dd`;
ELF SHA-256 is
`702a1fe78af38dbc3f913b1c6b7ff9cd74e97701310751feae8714997bc7940e`.
Main authenticated OTA passed 13 preservation checks without USB; each of the
three candidates passed 18 application-only USB update checks without erasing
flash. The complete host suite with 340 framework units, native build with
36 public-record layout checks and contracts passed. Live web verification
passed 29 checks and all 11 bodies completed, including four exact build assets,
large JSON/log responses and preserved security headers. Evidence is under
`test-results/add-latency-fix-2026-10-30/`.

All seven physical cases passed in one 1,373.775839-second run:

| Case | Duration |
| --- | --- |
| HDMI scan | 24.987485 s |
| Activity power cycle | 81.845978 s |
| Explicit switch input | 86.399398 s |
| Shared-wire source theft | 134.279441 s |
| Add Amity | 104.581229 s |
| Original two-owner consortium | 337.588307 s |
| Three-owner consortium | 604.059135 s |

`functional.json` and `functional-verification.json` record seven clean
entered testbeds, nine fixture activities created and deleted, four completed
Add transactions, four reviewed removals with scheduled-reset acknowledgments,
28 typed scan locations and no cleanup errors. Independent two-owner and
three-owner verifiers pass 21 checks each; the shared-wire verifier passes 20.
The consortium cases contain 38 fresh physical power assertions, 15 typed
locations, five initial routes and all eight cross-owner theft directions.
Their slowest Standby response was 69.750020 ms under the current one-second
limit. Shared-wire thief Standby completed in 66.950012 ms and selected-route
restoration in 183.450030 ms from the same physical trigger, both inside that
same one-second window. Selected activity and main boot remained unchanged.

Four clean Add samples in `add-timings.json` distinguish operation completion
from the later authenticated identity/configuration/settings proof:

| Add sample | POST to operation complete | Helper entry to first verified ready |
| --- | --- | --- |
| Standalone member 2 | 42.014 s | 79.202 s |
| Two-owner member 2 | 41.702 s | 79.083 s |
| Three-owner member 2 | 41.977 s | 80.022 s |
| Three-owner member 3 | 46.899 s | 84.557 s |

Matched clean 0.1.104 consortium samples improved by 9.173–10.270 seconds
(about 18–19%) from Add POST to operation completion, and by about 10–11%
from helper entry to first verified readiness. The clean 0.1.103 standalone
reference improved by 8.407 seconds from POST to completion. These are four
observations on this bench, not a latency distribution or guaranteed bound.
The 20-second discovery window still collects multiple candidates; final HTTP
proof takes about nine seconds after operation completion. Polling runs
concurrently with firmware progress, so accumulated HTTP request times are not
subtracted from the Add duration.

The changes remove three avoidable costs: boot tests only the configured
admission profile while explicit USB/native self-tests retain both; proposal
construction reuses exact validated `Identity`/`Peer` records instead of two
repeated signature checks; and the first known numeric authority-IP attempt
precedes blocking mDNS while retaining pinned TLS and failed-route fallback.
Untrusted certificate checks, profile matching, required member restart and
handshake remain. Separate boot-uptime measurements on three unclaimed boards
show Ready at 4.933–4.969 seconds, previously 10.643–10.679 seconds, about
5.7 seconds faster. `boot-startup-timing.json` records these update-time startup
measurements before the later explicit full USB self-test; they are not Add
elapsed times.

Remove now retains the exact admitted request/scope/revision permit when its
own saved-owner deletion publishes a newer local configuration. The terminal
exchange preserves original authority, full link/boot/group/epoch scope,
mutation/digest, target absence and its original two-second reset lifetime;
it never restamps the request. Native proof covers publication before terminal
send and 19 denied stale/foreign/expired permit cases. All four physical fixture
removals acknowledged scheduled reset. This reset lifetime is separate from
the one-second source-theft contract.

The final shipping-UI postcheck passes all 26 checks in
`main-final-postcheck.json`. Main boot `58584933c4556412aad1391c3b0de029`,
original authority/group, saved values, all 16 existing slot records and
metadata, names, hostname, hardware capabilities, Matter fabrics and HomeKit
pairing state were preserved except authorized cleanup and revision changes.
The main is ready and idle, without enrolled peers, removed-owner observations,
pending configuration/discovery, activity error or hardware error. A membership
diagnostic remains, and cumulative TX errors increased from 114 to 4,694;
RX drops remain zero. These scoped checks do not assert an absence of transport
or CEC transmission failures.

The earlier 0.1.104 timing baseline remains failed: Add completed 53.704 seconds
after POST and first verified readiness took 92.051 seconds from helper entry,
but reset acknowledgment was unconfirmed. Initial USB monitor attachment caused
a candidate reboot 41.879 seconds before POST. The saved logs expose the
retirement/configuration race (admitted link revision 196, cleanup revision 197);
this diagnostic sample is not the headline clean performance comparison.
`test-results/add-latency-2026-10-30/` retains `baseline-add.json`, its trace,
candidate log and native before-fix proof. Main failure captures remain in
`test-results/consortium-tls-memory-fix-2026-10-30/latency-baseline-failure-{snapshot.json,logs.txt}`.
Separate known-fixture post-failure recovery held member 2's reset button for
10.5 seconds and acknowledged release in `baseline-physical-recovery.json`;
it does not repair the failed report or create a pre-test reset requirement.
No USB/serial monitor was part of the 0.1.105 qualification run.

This qualifies all seven exercised cases on the normal admission profile,
main `amity-dut` and up to members `amity-dut-2`/`amity-dut-3`, one adapter per
owner, isolated front/back wires and simulated TV/AVR/player CEC responders.
Strict admission, four owners, adversarial radio/reconnect/loss/load,
four-adapter electrical timing and real picture/audio equipment remain outside
this evidence. Earlier failed physical reports remain failed.

## Previous one-second source-theft deadline — 30 October 2026

The then-current functional contract required thief Standby and resulting state
within one second of that component's successful physical Active Source
transmission. Shared-wire route restoration must complete within the same
original one-second window. The requested framework change is complete:
all 340 framework units pass, including exact one-second boundaries and
rejection of a response one nanosecond late. Command:
`PYTHONPATH=cec-harness python3 -m unittest discover -s cec-harness/unit_tests -q`;
the 14.088-second run is recorded in
`test-results/source-theft-one-second-2026-10-30/framework-tests.log`.
The subsequent complete 0.1.105 physical suite above exercises this one-second
contract, including both consortium sizes and shared-wire route restoration.
Earlier results below retain their then-current two-second contract and
recorded 2,000 ms bounds; they are not retroactively relabeled. The earlier
complete 0.1.104 host suite remains the separate 339-unit result recorded below.

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
