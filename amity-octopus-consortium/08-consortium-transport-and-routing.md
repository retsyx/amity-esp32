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

**1 November 2026 TLS reuse amendment.** Each authenticated peer has one
connection, independently of configuration readiness. Data exchange acquires
that connection implicitly; a pending or established connection is reused.
Concurrent acquisition does not create competing sockets. A same-peer,
same-boot duplicate cannot replace a healthy connection, including one still
synchronizing. A proven new boot or revoked credential retains the exact old
scope's invalidation. TLS handshake runs once per transport. Send and receive
reuse it. A wrapper wait may expire while preserving partial framing. A frame
that exceeds its progress, total or operational deadline is rejected once;
its remaining bounded bytes are drained without execution while TLS stays
available. Neither timeout extends the original frame's acceptance deadline.
EOF, fatal TLS/write failure, invalid length or malformed framing still makes
the stream unusable; authentication deadlines remain unchanged.

Hello/session authentication and configuration synchronization are separate
operations. A failed application synchronization leaves the connection open
and withholds readiness. The controller starts a fresh bounded synchronization
round on that stream using `synchronization` (kind 43), with a closed round ID,
phase and target revision. The member acknowledges the exact round before
hostname, configuration and owner-state transfer. Its ACK forms a TCP-order boundary
after abandoned transfer traffic. A new controller begin interrupts an older
member exchange; late replies cannot acknowledge a different round. Failed
rounds discard their transient transfer state and retry after one second,
recapturing current configuration rather than retaining an outgoing payload.
Only the original controller starts rounds; members reject failed rounds and
wait for its next begin. No ordinary activity or key work is replayed.
Membership transport owns asynchronous connection acquisition and recovery;
ordinary one-shot actions retain their existing unavailable-peer handling and
are never retained until a future connection becomes ready.

The restart barrier retains installation, controller/member identities, boots
and replay history. Each acknowledged round derives a fresh logical link ID
from its round ID on the same physical TLS connection. Before waiting for the
barrier, exact old-session execution is fenced; late callbacks for that ended
scope cannot close TLS. This fence preserves independent HDMI observations.
An expired causal IPC item, failed result delivery or failed context refresh
likewise ends its execution session and requests synchronization on the same
healthy transport. This retains exact cleanup without resending the lost work.
Only that typed exchange can reconcile different
envelope revisions after a partially applied synchronization; the target is
monotonic and replies must match its exact round and revision. Operational
messages retain their exact configuration fences. Beginning a round invalidates
execution leases/context and blocks positive work until the normal snapshot
exchange succeeds. A normal successful live configuration refresh keeps its
logical link and delivers admitted typed lookup results to their original
continuation. A recovery round ends the old continuation and discards its IPC
work rather than granting it replacement leases. Abandoned frames may only be
strictly validated and discarded, never executed. Identity,
membership and authority failure remain terminal and cannot be recovered by
loosening a check or treating a peer address as authority.

At startup, configured and available PSRAM owns all cJSON allocations, including
small nodes and strings. Its exhaustion returns allocation failure rather than
consuming the internal/DMA heap needed by authenticated transport and CEC.
Boards without PSRAM retain the normal allocator. The immutable hook is installed
before configuration parsing and application tasks. Allocation failures are
captured without allocating/logging in the heap callback and reported by the
existing service poll with exact request size, capabilities and heap headroom.

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

While a live refresh waits for its snapshot acknowledgment, an admitted typed
source-input result in the current old scope still completes its original
lookup continuation exactly once, before revision advance. Deliver it through
the same control hook and exact authenticated link as normal reception. Missing
or rejected delivery fails synchronization; it cannot be silently remembered by
replay protection and discarded. Obsolete-scope frames retain the existing
strict discard checks, and this handoff executes no positive owner intent or
restamped work.

Live configuration support is part of the execution capability floor. A peer
without it remains eligible for existing management/firmware-update operations,
not ordinary equipment execution. The release installer's existing required
capability floor remains unchanged so an older peer can install the update.

## 3. Ordering, input state and uncertainty

The first control-lease handshake queues the current nonemitting activity context before positive owner actions. This synchronizes ordinary follower replies/playback context without replaying an activity recipe or missed commands.

A context publication for a configuration that is still synchronizing is a
notification to recapture current nonemitting state after the existing revision
exchange and fresh lease. Do not retain its old payload in the action FIFO or
let its one-second action lifetime expire behind that exchange. The existing
owner-task callback supplies fresh current context; accepting that asynchronous
callback does not grant context readiness. Only admission of the current-scope
context to the link FIFO releases dependent actions behind it. Future-scope
positive work is rejected before admission. Already admitted work retains its
original FIFO position, lifetime and exact authorization fences; it is never
restamped or replayed. Current-scope discovery and exact key cleanup remain
independent of the presence of saved activity context.

**1 November 2026 setup-completion correction.** The original Python
`ControllerImpl.set_activity()` completes its existing power/input steps before
selecting the new activity. An asynchronous context recapture must preserve the
completion scope of that same in-progress setup: while `setup_pending` remains
true, its fresh context carries the current `setup_generation`, and the existing
owner expectation advances to the fresh sequence. The superseded future-revision
context was never queued and cannot produce its own completion snapshot. Once
setup completes, later context captures carry no setup generation. This changes
only transient completion bookkeeping; original CEC steps are neither repeated
nor retained for replay, and authenticated TLS, revision and boot fences remain.

`tests/owned_hub_settling_test.cpp` exercises the actual three-owner workers:
select immediately after applying configuration while both idle owners still
have the prior revision, recapture after each owner synchronizes, reject a stale
capture, and complete from the matching fresh pending-zero snapshots. The
regression fails on the old recapture generation and passes with the correction;
it also requires unchanged power-frame counts and non-setup captures afterward.
The existing original/local/remote settling comparisons remain in the same
host suite. This is software evidence, not physical four-device qualification.

The owner request includes its expected revision and reports a local typed
published/stale/failed completion. A stale revision does not publish or fence
the link. A genuine late publication failure fences only its still-current
captured link, boots, configuration, installation and authority scope. The
callback retains no mutable link pointer or old context payload.
Successful capture with no selectors completes configuration refresh without
granting context readiness, emitting a message or repeating empty captures.
This keeps ordinary discovery and reviewed removal available for a newly added
Amity before any activity refers to it.

Preserve causal operation order and button down/up state through bounded task/network handoff. Keep request deduplication and stale input/boot fences needed for message delivery; do not turn them into persistent activity approval. Already admitted held recipients and cleanup remain exact. Reconnect or authority refresh does not replay uncertain relative actions or restart a failed key.

Owner snapshots include volatile setup generation/last sequence/pending admitted completions and whether original expected replies remain pending. The controller unions those pending replies and uses the same original ActivitySettling quiet period and two-second deadline measured after the last admitted completion. Owners add no independent quiet period, timer or barrier. A typed source-input result completes a waiting original operation: its expiry fences the exact link. An owner snapshot is replaceable observation state: expiry drops that payload and requests a fresh publication of the latest owner state, including a final pending-zero completion. That status does not hold back new keys or ordinary commands, wait for CEC capacity or create a new frame queue.

A transport receipt reports only transport/IPC admission. It does not guarantee CEC FIFO admission, ACK, completion or equipment state. Return the actual result without hiding a second outgoing queue or automatically retrying ordinary work when CEC capacity opens. Do not wait for another key's ACK/completion before submitting new input or other original commands.

No durable current-activity resume ledger, global resource-plan reservation, prepare/commit execution barrier or persisted owner-profile approval is needed for ordinary equipment lookup. Configuration/storage journals and membership/reset transactions retain their separate existing purposes.

## 4. Backpressure and owner progress

Bound communication queues by explicit resource limits and report admission failure. Necessary IPC state handoff is permitted; retained outgoing frames, retry queues and hidden repeat backlogs are not. Keep owner input, RX frames, setup and discovery advancing while CEC transmissions are outstanding.

Operational TLS receive is incremental. Keep one bounded incoming frame/parser
per link and return to the existing IPC/lease pump while bytes are incomplete.
Both socket callbacks are nonblocking. Preserve the original 8 KiB frame cap,
two-second progress and ten-second framing bounds, and the five-second
operational receive budget beginning at the first socket/TLS readiness event,
including encrypted fragments that have not produced plaintext. Handshake and
snapshot wrappers retain their own bounded waits and yield at least one RTOS
tick while waiting. The existing send budget is unchanged. This incoming
communication state is not an outgoing CEC queue.

Application TLS writes are capped at 512 bytes to bound the pinned SDK's
PSRAM-to-DMA AES bounce allocations; the global TLS output/input capacities
remain 4,096/16,384 bytes, preserving certificate handshake and authentication
behavior. Peer frame bytes and parsing, admitted FIFO order, message lifetimes
and the existing three-second send budget/WANT handling remain unchanged.
This caps each write without introducing a queue or retry policy. Large
received TLS records may still require larger DMA bounce allocations.

Unexpected operational link closures emit one bounded diagnostic summary with a
static reason and phase, protocol error, peer/link/boot/revision identities and,
for expired causal IPC, its original kind, age and TTL. Admission-side expiry
and cleanup-capacity fences report the same metadata where the fence originates.
Admission captures immutable scalar scope/trace metadata under its existing
locks and emits only after those guards release; diagnostic formatting and
logging never run under the runtime or link-queue mutex. Normal
cancellation/retirement is distinguished. These logs contain no message
payload, credential, certificate or key, add no periodic traffic, and do not
change admission, deadlines, replay checks or reconnect policy.

The CEC FIFO has eight total queued-plus-active messages per wire. Releases have no extra slot; dropped full-FIFO releases are not retained or retried. Log every full-FIFO TX drop, including every member of a rejected atomic batch. Transport does not inspect CEC opcodes to make a different admission policy.

## 5. Discovery and status delivery

Owners publish changed observations and ordinary status for the original UI, with a bounded latest-report retry and handshake snapshot. Do not add periodic unchanged reports or an observation-age execution gate. The controller's cached observations are diagnostics and UI data; every power/key action must still reach the owner's actual local lookup. Preserve scope, distinguish runtime logical addresses from saved OSD selectors and retain owner-local physical-address selection. An offline report cannot rewrite a saved activity to disabled. Missing owners remain in membership until explicit removal.

Snapshot publication is demand-driven. When a queued snapshot expires, the link task passes only its adapter and exact controller/link/owner-boot/controller-boot/configuration scope to the existing synchronization task. A bounded flag per adapter requests recapture from the facade's latest owner snapshot cache; stale scope is discarded. Exact current-scope validation and flag selection share the synchronization mutex so a delayed stale notification cannot replace a current completion retry. The lookup takes runtime locks; runtime control hooks always execute after those locks are released. The expired payload is never replayed or given a new deadline. Admission failure likewise retains a recapture flag, not the failed payload. A newer observer update supersedes that flag and cannot be overwritten by an older send failing concurrently. Recovery uses the ordinary scoped result channel and existing bounded IPC, with no owner-task wait or outgoing CEC buffer.

The peer inventory takes its stable device ID and enrollment state from the durable roster. An incomplete TLS hello cannot replace that identity with an empty status ID. Its session scope is not read until the handshake publishes initialized status under the link mutex. Only matching authenticated, current-credential and exact-boot link metadata may overlay the roster; a ready link takes precedence over an incomplete parallel handshake. This is a status projection rule, not an additional equipment availability permission.

## 6. Existing management channels

Keep authenticated enrollment, hostname metadata, peer firmware update, per-device settings, explicit removal/reset and manual recovery. They are management operations with their existing reviewed-target, identity, boot, persistence and result semantics. They do not approve HDMI activity routes or permit members to host a UI.

Per-device adapter settings continue to use the selected owner's current boot, settings revision and configured slots, durable atomic apply and selected-device restart after result attempt. Explicit Remove retains the original-controller-only retirement/drain/reset-journal contract; offline removal cannot claim a remote reset. Firmware update retains exact release/image/job/target/offset correlation and independent target validation. See [settings/admin](12-control-surfaces-and-admin.md), [recovery](13-faults-removal-and-recovery.md) and [updates](14-ota-and-compatibility.md).

An admitted Remove retains its exact owner/request, full authenticated
`LinkScope`, membership revision and original two-second reset deadline in
`RemovalOperation`. Its own saved-configuration deletion may publish a newer
local revision before `finish_retirement` runs. Validate the issued pending
permit and original mutation/target/digest against the unchanged link scope;
do not reject solely because that local publication advanced, or restamp the
terminal request. Original-controller authority, exact boots/link/group/epoch,
TLS authentication, target absence from the roster and remaining lifetime still
apply. The receiver and acknowledgment retain their exact existing fences;
ordinary work remains revoked. The
[actual removal regression](../tests/consortium_removal_test.py) covers this
publication interleaving and stale, foreign, replaced and expired permits.
The complete 0.1.105 physical suite acknowledges scheduled reset for all four
fixture removals;
see [validation](VALIDATION.md) for its scope and retained failed baseline.

## 7. Validation

Test wrong owner/installation/controller, old boot/configuration/input, replay, malformed selectors, link loss, queue pressure and exact held cleanup without changing baseline CEC behavior. Compare local delivery with fake authenticated remote delivery of the same original intents. Management/security tests do not substitute for activity/CEC parity or real multi-device radio/CEC qualification.

The [actual TLS send regression](../tests/consortium_tls_receive_test.py)
checks the 512-byte write bound, exact frame bytes, partial writes, WANT
continuations and failures under the unchanged three-second total budget.
The separate passing 0.1.104 physical runs and their bench limits are recorded
in [validation](VALIDATION.md); this source-level test alone is not physical
qualification.

The 30 October runtime regressions execute the actual peer projection, link-task queue pump/admission and synchronization code: `python3 tests/runtime_peer_status_test.py`, `python3 tests/consortium_live_configuration_test.py`, `python3 tests/consortium_owner_repeat_test.py` and `sh tests/system_sync_test.sh`. They cover partial/obsolete peer status, snapshot versus typed-result expiry, final completion recapture, exact recovery fences and a newer observation arriving during an older failed send. Before/after failures are preserved under `test-results/device-removal-2026-10-29/{peer-status,causal-snapshot}-{before,after}-fix.log`; the concurrent-send regression is `test-results/consortium-runtime-fix-2026-10-29/snapshot-send-overwrite-{before,after}-fix.log`. These establish the transport invariants; they do not establish the cause or resolution of the physical three-owner source-theft failure.

Predeployment review also reproduced the partial-handshake scope read and a valid retry followed by a stale-link retry. Their failing/passing actual-code regressions are `test-results/consortium-runtime-fix-2026-10-29/{partial-scope,stale-retry}-{before,after}-fix.log`; the retry test checks that scope validation holds the same mutex as flag selection.

The later three-owner run on exact 0.1.100 passed its Main activity, both remote
theft suppressions and fresh Main Standby commands, then failed an owner proof
before Member 2's activity start. Later snapshots showed the same member boots
ready again, but the old firmware emitted no loop-close reason. This remains a
physical failure requiring new evidence; the separate native live-refresh
input-result regression establishes a lost-continuation bug, not its cause.
