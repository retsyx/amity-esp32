# Authenticated owner delivery

**27 October 2026 user amendment.** This layer transports original intents to the owning Amity. It must remain mechanically separate from the local activity and CEC algorithms. [Addressing](03-domain-model-and-addressing.md), [execution](07-activity-planning-and-execution.md), [membership](09-membership-security-and-enrollment.md) and [the original-controller policy](21-original-controller-policy.md) define its bounds.

## 1. Ownership and local dispatch

Resolve the selected stable Amity device ID through current authenticated membership. Deliver the typed operation or button-state change to that owner. The owner resolves the specified HDMI adapter, CEC side and OSD name using the original local discovery behavior and performs the original local operation.

Do not ship a periodic stream of key presses, a persisted CEC logical address, a newly compiled opcode policy or a preapproved topology as the recipient's activity model. The controller sequences the original activity decisions; each owner does the work that depends on its local equipment. Local and remote delivery must result in equivalent frames/order/budgets/admission/lifecycle.

For source input, the owner reports the actual local source-presence result with the original generation/sequence. A CEC FIFO drop after lookup still reports the actual presence; failed IPC admission has no lookup result. An asynchronous result stays bound to its original authenticated link, and delivery failure fences that link rather than redirecting it after reconnect. The controller can then continue the original explicit switch fallback/audio query. This is a bounded correlation for an in-flight typed request, not a CEC frame backlog, ACK wait, global barrier or persistent execution plan. Power and key dispatch do not depend on a controller-side discovery cache.

## 2. Authentication and framing

Keep authenticated exact-peer TLS, bounded framed messages, strict typed parsing, canonical stable IDs and existing membership/controller role checks. Only the durable original controller can issue operational work. Added devices remain headless, cannot become input gateways and cannot promote themselves.

Transport scope includes the current installation, original controller, target owner, appropriate boot/configuration/input context and bounded request identity. Derive control, realtime key-down and cleanup key-up authority from the typed action; retain its receiver deadline and exact link/boot identity. These classes and deadlines are volatile transport authorization, never saved activity permission or a per-button renewal protocol. Reject mismatched, stale, replayed, malformed or unauthorized work. Never derive authority from a hostname, IP address, discovery order or portable configuration alone. Do not import old trust or credentials during configuration restore.

The production wire codec and its closed schema own numeric kinds and bounds. Deprecated prepared-plan/profile/binding graph contracts are not a second path for actuation. Remove unused contracts with their implementation; unknown or unsupported kinds reject rather than silently reinterpret. Version/capability negotiation must not accidentally authorize an older incompatible behavioral policy.

Activity configuration saves preserve authenticated TLS links and HDMI discovery
observations. Peers negotiate `live_configuration` (bit `524288`) before using
the existing snapshot request/chunk exchange on a ready link. The controller
sends its current controller/revision slice using the previous envelope scope,
with the new revision in the bounded manifest. The member binds that revision
and sends its completion acknowledgment in the previous scope. Both then advance
the same session, and the member's owner-state slice uses the new scope. Link
ID, installation, controller/member identities, boots and replay history remain
unchanged. Applying the controller's local configuration does not wait for a
member acknowledgment or authorize replay of uncertain work.

Revision advance invalidates old positive-work leases; normal fresh grants use
the same existing issuance cadence and bounds. Queued work retains its original
configuration revision, creation time and deadline, and cannot be restamped into
the new scope. Obsolete in-flight frames may be discarded only after strict
identity, boot, epoch, role, capability, closed-payload, TTL and replay checks;
future revisions and mismatched scope remain rejected. A successful stale-frame
check means discard, never execution or lease renewal. These changes add no wire
kind, HTTP endpoint, ordinary command retry or alternate CEC policy.

Live configuration support is part of the execution capability floor. A peer
without it remains eligible for existing management/firmware-update operations,
not ordinary equipment execution. The release installer's existing required
capability floor remains unchanged so an older peer can install the update.

## 3. Ordering, input state and uncertainty

The first control-lease handshake queues the current nonemitting activity context before positive owner actions. This synchronizes ordinary follower replies/playback context without replaying an activity recipe or missed commands.

Preserve causal operation order and button down/up state through bounded task/network handoff. Keep request deduplication and stale input/boot fences needed for message delivery; do not turn them into persistent activity approval. Already admitted held recipients and cleanup remain exact. Reconnect or authority refresh does not replay uncertain relative actions or restart a failed key.

Owner snapshots include volatile setup generation/last sequence/pending admitted completions and whether original expected replies remain pending. The controller unions those pending replies and uses the same original ActivitySettling quiet period and two-second deadline measured after the last admitted completion. Owners add no independent quiet period, timer or barrier. A typed source-input result completes a waiting original operation: its expiry fences the exact link. An owner snapshot is replaceable observation state: expiry drops that payload and requests a fresh publication of the latest owner state, including a final pending-zero completion. That status does not hold back new keys or ordinary commands, wait for CEC capacity or create a new frame queue.

A transport receipt reports only transport/IPC admission. It does not guarantee CEC FIFO admission, ACK, completion or equipment state. Return the actual result without hiding a second outgoing queue or automatically retrying ordinary work when CEC capacity opens. Do not wait for another key's ACK/completion before submitting new input or other original commands.

No durable current-activity resume ledger, global resource-plan reservation, prepare/commit execution barrier or persisted owner-profile approval is needed for ordinary equipment lookup. Configuration/storage journals and membership/reset transactions retain their separate existing purposes.

## 4. Backpressure and owner progress

Bound communication queues by explicit resource limits and report admission failure. Necessary IPC state handoff is permitted; retained outgoing frames, retry queues and hidden repeat backlogs are not. Keep owner input, RX frames, setup and discovery advancing while CEC transmissions are outstanding.

The CEC FIFO has eight total queued-plus-active messages per wire. Releases have no extra slot; dropped full-FIFO releases are not retained or retried. Log every full-FIFO TX drop, including every member of a rejected atomic batch. Transport does not inspect CEC opcodes to make a different admission policy.

## 5. Discovery and status delivery

Owners publish changed observations and ordinary status for the original UI, with a bounded latest-report retry and handshake snapshot. Do not add periodic unchanged reports or an observation-age execution gate. The controller's cached observations are diagnostics and UI data; every power/key action must still reach the owner's actual local lookup. Preserve scope, distinguish runtime logical addresses from saved OSD selectors and retain owner-local physical-address selection. An offline report cannot rewrite a saved activity to disabled. Missing owners remain in membership until explicit removal.

Snapshot publication is demand-driven. When a queued snapshot expires, the link task passes only its adapter and exact controller/link/owner-boot/controller-boot/configuration scope to the existing synchronization task. A bounded flag per adapter requests recapture from the facade's latest owner snapshot cache; stale scope is discarded. The expired payload is never replayed or given a new deadline. Admission failure likewise retains a recapture flag, not the failed payload. A newer observer update supersedes that flag and cannot be overwritten by an older send failing concurrently. Recovery uses the ordinary scoped result channel and existing bounded IPC, with no owner-task wait or outgoing CEC buffer.

The peer inventory takes its stable device ID and enrollment state from the durable roster. An incomplete TLS hello cannot replace that identity with an empty status ID. Only matching authenticated, current-credential and exact-boot link metadata may overlay the roster; a ready link takes precedence over an incomplete parallel handshake. This is a status projection rule, not an additional equipment availability permission.

## 6. Existing management channels

Keep authenticated enrollment, hostname metadata, peer firmware update, per-device settings, explicit removal/reset and manual recovery. They are management operations with their existing reviewed-target, identity, boot, persistence and result semantics. They do not approve HDMI activity routes or permit members to host a UI.

Per-device adapter settings continue to use the selected owner's current boot, settings revision and presence generations, durable atomic apply and selected-device restart after result attempt. Explicit Remove retains the original-controller-only retirement/drain/reset-journal contract; offline removal cannot claim a remote reset. Firmware update retains exact release/image/job/target/offset correlation and independent target validation. See [settings/admin](12-control-surfaces-and-admin.md), [recovery](13-faults-removal-and-recovery.md) and [updates](14-ota-and-compatibility.md).

## 7. Validation

Test wrong owner/installation/controller, old boot/configuration/input, replay, malformed selectors, link loss, queue pressure and exact held cleanup without changing baseline CEC behavior. Compare local delivery with fake authenticated remote delivery of the same original intents. Management/security tests do not substitute for activity/CEC parity or real multi-device radio/CEC qualification.

The 30 October runtime regressions execute the actual peer projection, link-task queue pump/admission and synchronization code: `python3 tests/runtime_peer_status_test.py`, `python3 tests/consortium_live_configuration_test.py`, `python3 tests/consortium_owner_repeat_test.py` and `sh tests/system_sync_test.sh`. They cover partial/obsolete peer status, snapshot versus typed-result expiry, final completion recapture, exact recovery fences and a newer observation arriving during an older failed send. Before/after failures are preserved under `test-results/device-removal-2026-10-29/{peer-status,causal-snapshot}-{before,after}-fix.log`; the concurrent-send regression is `test-results/consortium-runtime-fix-2026-10-29/snapshot-send-overwrite-{before,after}-fix.log`. These establish the transport invariants; they do not establish the cause or resolution of the physical three-owner source-theft failure.
