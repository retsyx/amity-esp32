# Authenticated owner delivery

**27 October 2026 user amendment.** This layer transports original intents to the owning Amity. It must remain mechanically separate from the local activity and CEC algorithms. [Addressing](03-domain-model-and-addressing.md), [execution](07-activity-planning-and-execution.md), [membership](09-membership-security-and-enrollment.md) and [the original-controller policy](21-original-controller-policy.md) define its bounds.

## 1. Ownership and local dispatch

Resolve the selected stable Amity device ID through current authenticated membership. Deliver the selected activity or raw button edges to that owner. The receiver selects its current activity's audio/source equipment, then uses the shared local resolver for its configured adapter, side and OSD name.

Do not ship a periodic stream of key presses, a persisted CEC logical address, a newly compiled opcode policy or a preapproved topology as the recipient's activity model. Distribution delivers the selected activity and equipment ownership. Each Amity runs the original local activity algorithm for its own equipment. Local and remote delivery must retain equivalent local frames, order, attempt budgets, admission and key lifecycle.

Activity lookup and CEC transmission completion are sequential within each Amity's local activity job. Its old-only Standby steps finish before its new Power On steps. Different Amities progress independently: a missing device, scan, transmission or failed command on one Amity cannot hold another Amity's activity. The distribution layer neither advances individual CEC steps nor waits for remote device responses or step-completion reports. The owner loop continues RX, discovery, input cleanup and completions while its own activity call waits.

Source lookup, explicit switch-input fallback and source-theft handling use the original owner-local algorithm and current local activity. A source on another Amity cannot authorize or suppress local fallback, and a controller-side discovery cache cannot prevent owner-local power or key lookup. No cross-owner CEC dependency, outgoing frame backlog or persistent execution plan is introduced.

Every owner retains the shared selected activity as current context, including
an owner with no equipment roles in it. Only its own equipment's CEC recipe is
executed. Such an owner keeps the existing foreign-source policy: it can issue
directed Standby to a local thief without routing a foreign source. Only a
shared Standby selection means that the whole system has no current activity.
Reconnect and configuration recapture deliver nonemitting context; they never
start or repair an activity automatically.

An owner's transient activity job retains its admitted original-main
authentication scope. Its local typed steps use a local activity-instance
guard so deferred work cannot continue a canceled job after authority loss.
That guard never crosses the network and requires no renewal. Starting the
job ends the prior registered held streams; its nonemitting shared context is
committed even when no equipment role belongs to that owner. Matching shared
generation and equal role selections identify same-target repair locally;
distinct activities sharing equipment do not gain extra Power On commands.

## 2. Authentication and framing

Keep authenticated exact-peer TLS, bounded framed messages, strict typed parsing, canonical stable IDs and existing membership/controller role checks. Only the durable original controller can issue operational work. Added devices remain headless, cannot become input gateways and cannot promote themselves.

**1 November 2026 ordered-control amendment.** The authenticated connection from the durable original main supplies authority. There are no input/control execution grants, permission renewals, generic frame identifiers or replay caches, random hold tokens, or per-key wire sequence checks. TCP/TLS supplies ordered delivery. Ordinary input carries only its activity generation and raw button edges; the authenticated envelope supplies the destination Amity. No adapter, side, OSD name, resolved CEC recipient or per-key authority is transported. The member's existing ordered task handoff preserves down/up order. Boot, configuration and activity changes still invalidate stale work; malformed or unauthorized scope and expired concrete requests are rejected. Never derive authority from a hostname, IP address, discovery order or portable configuration alone. Do not import old trust or credentials during configuration restore.

The selected activity and current boot/configuration scope identify ordinary activity work. Owners do not exchange per-step completion identities or results. Local CEC completion instances still prevent an old transmission completion from continuing a newly pressed key; they are never transported as key tokens.

`key_edges` contains one `{key,down}` edge, or an atomic same-key down/up pair for a finite tap. The pair enters existing typed IPC as one item so its up cannot erase the admitted tap before one press. The main keeps only each raw key's owner index. The receiver keeps one compact owner-level key table; repetition follows its existing local successful-completion pacing. Only state edges cross the network. A pure up retains cleanup capacity and its two-second transport lifetime; down and complete taps use the ordinary half-second input lifetime. No repeat messages, recipient tables or per-key configuration copies are sent or saved.

Add, Remove, adapter settings and firmware-update payloads retain their actual operation/request identities so a lost response can be reconciled safely. Those operations keep their concrete transaction deadlines and reviewed configuration/boot guards. Ordinary activity or key traffic needs no renewable authorization or generic frame identity.

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

The restart barrier retains installation, controller/member identities and boots. Each acknowledged round derives a fresh logical link ID
from its round ID on the same physical TLS connection. Before waiting for the
barrier, exact old-session execution is fenced; late callbacks for that ended
scope cannot close TLS. This fence preserves independent HDMI observations.
An expired causal IPC item or failed context refresh
likewise ends its execution session and requests synchronization on the same
healthy transport. This retains exact cleanup without resending the lost work.
Only that typed exchange can reconcile different
envelope revisions after a partially applied synchronization; the target is
monotonic and replies must match its exact round and revision. Operational
messages retain their exact configuration fences. Beginning a round invalidates
the old execution context and blocks positive work until the normal snapshot
exchange succeeds. A normal successful live configuration refresh keeps its
logical link and preserves admitted owner-local continuations in their original
scope. A recovery round ends the old continuation and discards its IPC
work rather than replaying it in the new scope. Abandoned frames may only be
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
ID, installation, controller/member identities and boots remain unchanged. Applying the controller's local configuration does not wait for a
member acknowledgment or authorize replay of uncertain work.

Queued work retains its original configuration revision, creation time and
deadline, and cannot be restamped into the new scope. Obsolete in-flight frames
may be discarded only after strict identity, boot, epoch, role, capability,
closed-payload and concrete TTL checks; future revisions and mismatched scope
remain rejected. A successful stale-frame check means discard, never execution.
These changes add no wire kind, HTTP endpoint, ordinary command retry or
alternate CEC policy.

Live configuration refresh keeps an admitted local lookup or transmission continuation under its original scope until that owner completes its existing work. It does not create cross-owner completion traffic or restamp work into a later revision. Obsolete-scope frames retain strict discard checks.

Live configuration support is part of the execution capability floor. A peer
without it remains eligible for existing management/firmware-update operations,
not ordinary equipment execution. The release installer's existing required
capability floor remains unchanged so an older peer can install the update.

## 3. Ordering, input state and uncertainty

Successful application synchronization queues the current nonemitting activity context before positive owner actions. This synchronizes ordinary follower replies/playback context without replaying an activity recipe or missed commands.

A context publication for a configuration that is still synchronizing is a
notification to recapture current nonemitting state after the existing revision
exchange. Do not retain its old payload in the action FIFO or
let its one-second action lifetime expire behind that exchange. The existing
owner-task callback supplies fresh current context; accepting that asynchronous
callback does not grant context readiness. Only admission of the current-scope
context to the link FIFO releases dependent actions behind it. Future-scope
positive work is rejected before admission. Already admitted work retains its
original FIFO position, lifetime and exact authorization fences; it is never
restamped or replayed. Current-scope discovery and exact key cleanup remain
independent of the presence of saved activity context.

**1 November 2026 independent-owner amendment.** Configuration/context recapture preserves current local activity state and never repeats emitted CEC work. Each owner advances its original local activity algorithm from its own lookup and CEC transmission completions. Another owner's progress, device state or observation snapshot is not an activity condition. Tests must cover cold lookup and configuration recapture, old-only Standby before new Power On locally, independent progress across owners and unchanged CEC frames/attempt budgets.

Pending `find_device()` continuations resume in their existing deferred arrival
order, including when a later lookup on another adapter or side finishes first.
A command resolved immediately from the cache never enters that pending-lookup
order and remains immediate. The owner continues pumping received frames,
discovery and transmission completions while the earlier lookup waits.

The owner request includes its expected revision and reports a local typed
published/stale/failed completion. A stale revision does not publish or fence
the link. A genuine late publication failure fences only its still-current
captured link, boots, configuration, installation and authority scope. The
callback retains no mutable link pointer or old context payload.
Successful capture with no selectors completes configuration refresh without
granting context readiness, emitting a message or repeating empty captures.
This keeps ordinary discovery and reviewed removal available for a newly added
Amity before any activity refers to it.

Preserve causal operation order and button down/up state through bounded task/network handoff. Keep actual operation idempotence and stale input/boot fences needed for message delivery; do not turn them into persistent activity approval. The receiver looks up the current activity target again for press, repeat and release; it retains no per-key CEC recipient. Reconnect or authority refresh does not replay uncertain relative actions or restart a failed key.

Activity setup waits for CEC transmission completion, including the wire ACK
that is part of the supplied attempt budget. It does not query or wait for
reported power, active-source state, a CEC application reply, a device-response
quiet period or a second settling deadline. No per-step completion report
crosses between Amities.
Owner snapshots remain replaceable observation state; expiry drops the payload
and requests a fresh publication of the current state. A snapshot does not
advance the activity or hold back ordinary commands, wait for CEC capacity or
create a new frame queue.

A transport receipt reports only transport/IPC admission. It does not guarantee CEC FIFO admission, ACK, completion or equipment state. Return the actual result without hiding a second outgoing queue or automatically retrying ordinary work when CEC capacity opens. Do not wait for another key's ACK/completion before submitting new input or other original commands.

No durable current-activity resume ledger, global resource-plan reservation, prepare/commit execution barrier or persisted owner-profile approval is needed for ordinary equipment lookup. Configuration/storage journals and membership/reset transactions retain their separate existing purposes.

## 4. Backpressure and owner progress

Bound communication queues by explicit resource limits and report admission failure. Necessary IPC state handoff is permitted; retained outgoing frames, retry queues and hidden repeat backlogs are not. Keep owner input, RX frames, setup and discovery advancing while CEC transmissions are outstanding.

Operational TLS receive is incremental. Keep one bounded incoming frame/parser
per link and return to the existing IPC pump while bytes are incomplete.
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
change admission, deadlines or reconnect policy.

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
A reset acknowledgment can follow observations/results that the member sent
before revocation on the same ordered TLS stream. Keep the session restricted
while draining only valid heartbeat, device-status, owner-snapshot, settings-result
and update-result frames. Their authenticated peer, boots, epoch, role,
capabilities, closed payload and concrete TTL still apply. A positive current
or older configuration is permitted only for this nonexecuting discard;
future configurations are rejected. The reset acknowledgment itself retains
its exact current configuration and mutation/target/digest correlation. No
management/readiness grant, new request, retry or CEC coordination is involved.
The original two-second deadline and 32-frame receive bound are unchanged.

The 0.1.121 Add cleanup reported an unconfirmed reset even though the member
logged successful retirement and then rebooted. The previous drain accepted
only heartbeat/settings-result traffic, so a valid owner snapshot or device
status ahead of the acknowledgment could terminate the receive early. The
[removal regression](../tests/consortium_removal_test.py) now constructs the
snapshot with the production codec and places current/older typed observations
before the acknowledgment; it also checks wrong scope, future configuration,
malformed payload, missing capability, obsolete acknowledgment and bounded
drain rejection. Native ASan/UBSan validation reproduces the early failure with
the saved old reader in
[removal-drain-native-before.log](../test-results/remote-keys-final-2026-11-01/removal-drain-native-before.log).
The current full removal regression passes in
[removal-drain-native-after.log](../test-results/remote-keys-final-2026-11-01/removal-drain-native-after.log),
including the original admission, durable reset marker, exact acknowledgment,
stale-scope and deadline checks. Physical outcomes remain separate in the
[0.1.121 report](../test-results/remote-keys-final-2026-11-01/all121-progress.json)
and subsequent run reports; the 0.1.121 logs do not identify the exact preceding
frame kind.

The complete 0.1.105 physical suite acknowledges scheduled reset for all four
fixture removals;
see [validation](VALIDATION.md) for its scope and retained failed baseline.

## 7. Validation

Test wrong owner/installation/controller, old boot/configuration/input, malformed selectors, link loss, queue pressure and exact held cleanup without changing baseline CEC behavior. Compare local delivery with fake authenticated remote delivery of the same original intents. Management/security tests do not substitute for activity/CEC parity or real multi-device radio/CEC qualification.

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
