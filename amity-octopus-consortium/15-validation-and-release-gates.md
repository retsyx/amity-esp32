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

Retain enrollment Security2/physical-window, exact-leaf TLS, durable original role/headless boot, settings generation/atomic-save/response-attempt restart, explicit removal/reset and exact-image update checks. Those are separate management/security contracts and do not justify persisted HDMI route approval.

## User interface

Preserve approved tab/form/layout behavior and original activity controls. Device choices include actual owning hostname and stable adapter context while carrying complete qualified identity. Missing choices stay saved. There is no Enable/Approve-path control, disabled import workflow, receiver-profile wizard or member UI. Verify backend dispatch as well as rendered controls.

## Physical qualification

Host/ASan/UBSan tests establish software behavior within their fixtures. Separate hardware evidence must cover actual ESP32-to-ESP32 transport, real Siri/Atomic input, local/remote CEC wire captures, all configured adapter pairs and simultaneous load, radio/HTTPS/scan pressure, full FIFO and loss/reconnect, detachable hardware and real AVR/display/source picture/audio. CEC ACK alone is not AV success.

Preserve existing production gates for secure boot/storage, release signing, provisioning/recovery and ecosystem pairing retention. Current four-adapter profile and three-device software limits are not measured product-capacity promises.

## Evidence record

Record exact source commit, build/configuration identity, SDK/dependency pins, test commands/results and artifact hashes. Label historical graph-model tests as historical; do not use them to claim the new selector model is tested. Update [VALIDATION.md](VALIDATION.md) and `IMPLEMENTATION_STATUS.json` only with checks actually run. The current task's final result must distinguish software/build evidence from physical device testing.
