# Validation of original behavior through owner delivery

The acceptance target is original local Amity behavior with qualified ownership. A build, Ready endpoint, retained fields or successful transport handshake cannot establish that target. [AGENTS.md](../AGENTS.md), [source baseline](19-source-baseline.md), [CEC behavior](06-cec-roles-and-message-policy.md) and [execution](07-activity-planning-and-execution.md) apply.

## Behavioral comparison

Feed identical activities, observations and input events to baseline `98e64a6`, qualified all-local handling and qualified remote-owner handling. Compare emitted wire, initiator, destination, payload, ordering, attempt budgets, admission results and key lifecycle. Transport timing is not permission to change original cadence, timeouts, discovery/recovery or queue behavior.

Required cases:

- Selective power transitions and same-target repair, one device used in several roles, TV/source power differences, original vendor quirks, standby and original completion-paced activity-settling/input readiness.
- Separate AVR/audio equipment, source routing, missing source with explicit switch-device/input fallback and non-CEC source use. Power/key work reaches the owner even with no coordinator observation; source presence comes from owner lookup, and FIFO overload must not masquerade as absence.
- Missing owner/adapter/OSD name and later return with zero activity-enable/configuration mutation; identical OSD text on separate owners/adapters/sides.
- Per-adapter original allocation, back-first discovery, lowest usable detected front PA or saved fallback, fixed-for-boot selection, received-frame effects and discovery-triggered original handling.
- Independent simultaneous keys, completion-paced repetition, new input during outstanding ACK, finite taps, aggregate recipient release, failed-key termination and matching late-up no-op.
- Eight total active/queued CEC messages per wire, immediate final up, permanently dropped full-FIFO release, every dropped TX logged and no outgoing executor/retry backlog.
- Original startup/accepted-selection wait-for-release gate, swallowed hold behavior, Siri-only Power macros, unchanged swipe/tap recognition and generated event sequence.

Use meaningful transcript/integration assertions. A test that merely expects new graph-policy types or repeats the implementation is not evidence that original behavior was preserved.

## Persistence and migration

Canonical schema 3 contains selectors and user intent, with no enabled, executable route, binding/domain/resource/capability/PA approval graph or durable activity-resume ledger. Migrate v1/v2 while retaining stable IDs where present, all role/switch/quirk fields, order, slots and aliases. Ignore old enabled flags. Inject storage cuts and verify backup/identity/trust/journal consistency without restoring runtime addresses, input or grants.

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
13-line test constructs `DUT("amity-dut")`, a TV, AVR and player, then uses
`with TestBed(...): dut.scan_hdmi().assert_contains(...)`. DUT construction
implicitly reads the wiring document. Shared configuration, test-bed, runner
and discovery modules own CLI options, per-DUT credentials/URLs/TLS,
authentication and identity checks, cleanup, UI operations, evidence capture
and scan assertions. The resource interface supports multiple DUTs without
adding another functional scenario. Component protocol `handle()` methods
remain in the common framework. Generic private reports contain `test`,
`passed` and a `testbeds` list; tests contain no reporting helpers.

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

The initial framework contains exactly one functional scenario: `amity-dut`
scans a TV, AVR and playback device. It requires fresh wire identity exchanges
and matching UI names/types/addresses. Wire captures identify the actual DUT
initiators; legacy aggregate UI address fields are not owner-local addressing
evidence. This scan passed on firmware `0.1.84` on 28 October 2026; the framework
README records the evidence location and scope. It does not replace the broader
behavioral comparison or physical qualification gates above.

## Evidence record

Record exact source commit, build/configuration identity, SDK/dependency pins, test commands/results and artifact hashes. Label historical graph-model tests as historical; do not use them to claim the new selector model is tested. Update [VALIDATION.md](VALIDATION.md) and `IMPLEMENTATION_STATUS.json` only with checks actually run. The current task's final result must distinguish software/build evidence from physical device testing.
