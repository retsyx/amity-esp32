# 08 — Consortium transport and routing

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

**Status:** proposed protocol v1 within architecture proposal v0.1. This document owns the operational wire contract. Enrollment cryptography is in [09](09-membership-security-and-enrollment.md); BLE transport is in [10](10-ble-and-matter-coexistence.md).

## 1. Transport choice

Use one persistent mutually authenticated TLS/TCP connection from each member to the designated coordinator. The proposed configurable service port is 7443, separate from administration HTTPS. Advertise `_amity-group._tcp` with protocol range and nonsecret discovery information. mDNS, cached IPs and router addresses are connection hints, never identity proof.

The coordinator routes member observations into the planner and sends authorized work directly to its owner. Members do not open arbitrary actuation connections to one another. No broker, UDP broadcast command bus, ESP-NOW dependency or CEC frame flood is required. Local coordinator-to-actuator calls implement the same semantic admission/result interface without a network round trip.

This choice keeps persistent sessions and trust relationships bounded for small home systems. It creates a deliberate coordinator availability dependency. Direct peer operations or automatic failover require a new authority design, not merely opening more sockets. [Consortium architecture](02-consortium-architecture.md), [recovery](13-faults-removal-and-recovery.md).

The selected TLS backend, cipher policy, enrolled leaf trust anchors and mandatory exact key pins follow [security](09-membership-security-and-enrollment.md). Do not reuse browser certificate exceptions or administration passwords for operational peer trust.

## 2. Connection establishment

Operational connection states are `disconnected → connecting → authenticating → negotiating → synchronizing → ready`. Failures return to bounded backoff with jitter. Backoff is capped and reset only after a healthy session, so a reboot loop does not storm the coordinator.

After TLS key proof, exchange a bounded transport `HelloFrame` before normal envelopes. Each side provides protocol minimum/maximum, ConsortiumId, OctopusId, its current per-boot BootId, coordinator epoch, config revision/hash, firmware identity, required/optional capability flags and accepted frame-size cap. A reconnect does not generate a new BootId. The coordinator assigns a random `link_id` for this connection. These messages are accepted only on an authenticated connection and must agree with the enrolled identity; application-supplied IDs cannot override the authenticated peer.

The initial hello is deliberately separate from the normal envelope because the peer's current BootId is not yet known. It never authorizes CEC work. Reject incompatible major versions or required capabilities with a bounded error, and expose the reason in administration. Capabilities are a closed, versioned registry; arbitrary strings do not activate code paths.

Synchronization establishes current membership/authority, applied compiled policy, relevant topology/binding revisions and observation cursors. A member that was offline cannot resume queued actuation from its last connection. Create new execution leases only after synchronization. Duplicate simultaneous connections for the same OctopusId are resolved by an explicit single active link; invalidate the displaced link's leases before accepting work on its successor. A second claimant with the same ID but a different pin is an identity conflict, not automatic replacement.

## 3. Framing and parsing

Each application frame is a four-byte unsigned big-endian length followed by UTF-8 JSON. Proposed initial maximum is **8192 bytes including the JSON body but excluding the length prefix**. Reject zero, oversized, malformed or incomplete frames before allocating an unbounded buffer. Use bounded incremental reads and a progress deadline; close peers that repeatedly violate framing.

Use strict JSON: reject duplicate keys, invalid UTF-8, embedded NUL, trailing content, unexpected fields in closed objects, excessive nesting, integer overflow and invalid enum values. Compression is not part of protocol v1. Parse outside the timing/CEC worker path.

Large configuration/diagnostic transfers use explicit chunk messages with transfer ID, index, total length, whole-object hash and bounded encoded chunk data. Initial encoded data cap is 4096 bytes per chunk, also constrained by the 8192-byte total envelope. Check total size before staging storage. Bound simultaneous transfers per peer and globally; do not materialize both a complete JSON/base64 blob and many decoded copies in internal RAM.

Firmware images use the OTA mechanism described in [14](14-ota-and-compatibility.md), not giant operational envelopes. Chunking does not grant permission to saturate the control channel. Diagnostic/transfer traffic yields to control messages.

## 4. Canonical envelope

After hello, every message uses these exact field names:

```json
{
  "protocol_version": 1,
  "consortium_id": "10000000000000000000000000000001",
  "message_id": "60000000000000000000000000000001",
  "origin_octopus_id": "20000000000000000000000000000001",
  "origin_boot_id": "30000000000000000000000000000001",
  "destination_octopus_id": "20000000000000000000000000000002",
  "destination_boot_id": "30000000000000000000000000000002",
  "coordinator_epoch": "1",
  "config_revision": "7",
  "kind": "operation",
  "ttl_ms": 1000,
  "payload": {}
}
```

This example shows the envelope only; an empty operation payload is invalid. Valid complete fixtures accompany the package. IDs and uint64 strings follow [document 03](03-domain-model-and-addressing.md). `ttl_ms` is an integer 1–10000 for operational messages; individual kinds may impose a smaller cap. Long scans/transfers have their own job lifetime and exchange multiple short-lived messages.

`origin_octopus_id` is the authenticated author, never a copied user-input source. Only the original controller turns local remote/browser/integration intent into semantic operations. Members supply observations/results and explicitly authorized opaque enrollment relay bytes; they cannot originate user intent or impersonate the controller.

`destination_boot_id` must match the recipient's current boot. No broadcast destination, wildcard node or wildcard boot is permitted for actuation. A message from an unexpected consortium or coordinator epoch is rejected even if its TLS key is otherwise enrolled.

## 5. Message kinds and authority

| Kind | Authorized sender → receiver | Meaning |
|---|---|---|
| `heartbeat` | Either active connection peer | Liveness and bounded status/cursor summaries. Not an execution grant. |
| `settings_read` (38), `settings_write` (39), `settings_result` (40) | Current original controller → exact ready owner; owner → controller | Negotiated `per_adapter_settings` capability 32768. Read metadata only, or exact boot/settings/presence-generation checked durable preference save with explicit target restart; closed payload contract below. |
| `device_status` (37) | Authenticated member → original controller | Closed `{hostname}` metadata, negotiated only with `hostname_metadata` capability 16384. It reports the actual canonical network name and grants no control authority. |
| `snapshot_request`, `snapshot_chunk` | Role-appropriate coordinator/member | Resynchronize committed status or compiled policy; strict transfer limits. |
| `observation` | Member → coordinator | Scoped observations with origin boot and sequence. |
| `binding_update`, `binding_result` | Coordinator → owner; owner → coordinator | Atomic, revision-checked synchronization of current name-to-address resolutions and its acknowledgment; no configuration edit or CEC side effect. |
| `plan_prepare`, `plan_barrier`, `plan_abort` | Coordinator → affected owner | Reserve/check, fence old work, or cancel prepared work. |
| `plan_result` | Affected owner → coordinator | Correlated preparation/barrier/abort result, including capacity and generation. |
| `execution_lease` | Actuator → coordinator | Receiver-local, bounded permission to deliver operations for the current synchronized link. |
| `operation` | Coordinator → actuator | One semantic local operation against a qualified binding. |
| `operation_result`, `operation_query` | Actuator → coordinator; coordinator → actuator | Distinct admission/completion/observation outcomes or lookup by CommandId. |
| `hold_release` | Coordinator → actuator | Communicate button up for the exact admitted hold token; cleanup never creates a new positive action. The former `hold_renew` kind/slot 4 is retired and must not be reused. |
| `hold_result` | Actuator → coordinator | Hold accepted/released/expired or rejected for its exact token. |
| `scan_request`, `scan_event` | Coordinator → scanner; scanner → coordinator | Bounded scan job admission, progress and completion, without activity authority. |
| `config_prepare`, `config_activate`, `config_result` | Coordinator ↔ affected owner | Versioned staging/activation with explicit results. |
| `membership_event` | Membership authority → member | Authenticated retirement/roster state; negotiated explicit `factory_reset` removal resets only the selected committed member. Generic retirement never erases. Never promotes or replaces the original controller. |
| `membership_result` | Member → original controller | Idempotent retirement/status acknowledgement; `reset_scheduled` means the existing factory-reset marker is durable, not that erasure has completed. Coordinator promotion/handover is unsupported. |
| `enrollment_control` | Provisional candidate ↔ enrollment authority | LAN proof, finalization, status and abort for the exact prepared join; no ordinary member privilege. |
| `enrollment_relay` | Authorized relay ↔ authority | Opaque bounded commissioning transport, only in an authorized session. |
| `error` | Either peer | Correlated, bounded protocol error without secrets. |

This is the v1 kind registry. Owning implementation documents define payload fields; their schemas must be added before a kind is implemented. The provided envelope schema validates registry and common fields, plus the `operation` and `device_status` payloads. It intentionally does not claim full payload validation for every kind.

Unknown kinds or fields are errors in v1. Extensions require explicit capability negotiation; do not send `device_status` unless both peers negotiated `hostname_metadata` (16384). Hello is unchanged, and older peers retain their compatible management/update path without receiving the new kind. A member cannot send `operation` to another member or originate a user intent. Only the original controller plans operations. A misrouted target never triggers an automatic route to a different owner.

Old handover/prepared-successor state and messages grant no authority in this profile. Startup remains passive for explicit original-controller recovery; it never finishes a legacy promotion. Only old transfer-marker presence is checked to force passive recovery; there are no transfer wire kinds or decoder/state-machine paths. The current known-capability mask is 258047, including `original_controller_only` 8192, hostname metadata 16384, per-adapter settings 32768, removal reset 65536 and `owner_repeated_keys` 131072. The owner-repeat capability is required for execution/profile synchronization: older peers remain management/update-compatible and quarantined from positive execution until upgraded. Recognition of the old token-holds capability alone does not establish the new semantics.

## 6. Operation payload

Every `operation` contains:

```text
command_id, command_sequence, plan_id, session_id, route_generation
component_id, binding_id, binding_revision
domain_id, domain_revision
target: FullyQualifiedCecAddress
execution_lease_id
operation: closed semantic operation enum
arguments: operation-specific closed object
```

The envelope destination must equal `target.octopus_id`, the consortium must match, and the binding must match the entire endpoint plus component/domain/revisions. `operation` is initially one of `ensure_power`, `select_route`, `announce_active_source`, `key_press` or `query_state`. Release uses its dedicated token message so emergency cleanup need not fabricate a new ordinary command. Capability/role policy can reject any operation even if its enum is recognized.

`ensure_power` arguments are `{ "on": true|false }` and require a verified discrete/idempotent operation. Never implement it with a toggle, observed or unobserved. A deliberately requested toggle is a separate one-shot key intent with uncertain-outcome/no-replay semantics, or is rejected as unsupported; it is not power reconciliation. `select_route` takes a stable `route_id` from the prepared local plan; `announce_active_source` has an empty arguments object and targets an admitted proxy binding. `key_press` includes key code, `hold_token` and `repeat`: true installs a held-down state whose CEC repeats are produced locally; false is one press/release. The closed payload has no per-button lease field. `query_state` names a supported field such as power or audio. Detailed admissible fields are in the companion schema.

The target is a control binding, not a raw CEC header. For appliance bindings it names the destination appliance. For proxy bindings it names the locally owned identity representing the selected component. The local role compiler chooses wire initiator/destination, including a broadcast destination where the specific semantic operation permits it. The network cannot request arbitrary raw frames through this API.

### Name resolution and runtime binding updates

Portable component configuration and admin device selection carry `QualifiedDeviceName` with `osd_name`. `operation.target` remains `FullyQualifiedCecAddress` with the already resolved `logical_address`; an actuator never substitutes a fresh address into an old operation. Its native binding records the exact configured `device_name` that discovery resolved. Proxy targets are locally allocated role identities, not native OSD matches.

When a unique match for an unchanged selector moves LA, the observing owner immediately fences all old capability bindings for that selector and reports evidence. The coordinator sends `binding_update` under the current authority/configuration with a `resolution_id`, the affected old/new binding revisions and records, current domain revisions, and the exact observation boot/sequence references. The update atomically covers all capability bindings derived from that selector. The actuator compares expected old revisions, validates its current observations and configured names, installs the new runtime bindings, and returns `binding_result` with the resolution ID and applied binding/revision set or a bounded rejection. Duplicate identical updates are idempotent; conflicting or stale updates fail. No power, routing or held input is emitted by this exchange. The coordinator admits fresh work only after acknowledgment; lost acknowledgments are resolved through snapshot/status synchronization, never by sending an operation to test the new address.

The persisted selectors and `config_revision` stay unchanged; binding and affected domain allocation revisions provide the runtime fence. Reconnect snapshots carry the current agreed resolution separately from portable configuration. An offline member may collect evidence but cannot grant itself group authority. Schema coverage for these two payloads must be completed in W13 before enabling them, with malformed, partial-update, lost-result and old-target rejection cases.

## 7. Freshness, deadlines and execution leases

`ttl_ms` alone is insufficient: starting a fresh timer on arrival would make a long-delayed network packet appear new. Use both bounded local queue residence and a **receiver-issued execution lease**.

After synchronization, the actuator issues a random lease ID with a deadline on its own monotonic clock. It binds the lease to current TLS `link_id`, both BootIds, coordinator identity/epoch, applied config revision and operation class. The `control` class has proposed validity 5000 ms and a hard cap of 10000 ms. The `realtime_input` class, required for initial key-press delivery, has proposed validity 500 ms and a hard cap of 2000 ms. Renew general execution grants while relevant work is active; retain at most two unexpired IDs per class during rollover. These are distributed delivery/authority qualification targets, not per-button timers. Disconnect, restart, authority change or incompatible policy change invalidates all leases.

An operation is admitted and its first press begins only while its referenced lease remains valid locally. The coordinator treats the advertised duration conservatively and requests/reuses new authority after delay. It does not calculate a recipient deadline from its own monotonic timestamp. A delayed packet cannot extend an already issued lease by arriving late. Expired undelivered operations require a new planning/admission decision, not a rewritten TTL on the same stale action. Once a held-down state is admitted and started, its initial delivery deadline does not expire that state. Further owner-local CEC attempts use current acknowledged plan/route/link authority and unchanged binding context, following the original completion-paced loop. General authority maintenance does not transmit another key press or renew the button state.

Member-to-controller user inputs and all per-button renewals are forbidden. The original controller preserves its own input context and original queue-admission deadline before planning. It cannot freshen delayed local input merely by issuing a new actuator lease. The protocol has no member user-intent, ingress-lease or hold-renew kind; unknown/retired kinds are rejected.

For ordinary message TTL, each process accounts for time spent in its own queues and discards expired work. A new semantic envelope created by the coordinator from an input must preserve the causal request's remaining admissible age and sequence/session validity. Network transmission time is not inferred from unsynchronized clocks; original-controller input admission and actuator execution leases supply local age bounds. Exact-token release/cancel is cleanup and is not blocked merely because its positive-input lease expired; it cannot create a new action or release a different token. Each remote plan/hold is fenced by its exact authenticated link as well as peer/boot/session identity. Disconnect synchronously retires that link's held state, and a delayed old-link callback cannot cancel a new connection's state. Silent stalls fall back to general plan/authority liveness; no button or relative operation is replayed on reconnect. [Held-key execution](07-activity-planning-and-execution.md).

## 8. Ordering, acknowledgements and uncertainty

TCP orders bytes on one connection, not events across connections, queues, tasks or boots. Observations include per-origin boot sequence numbers; stale observations cannot overwrite newer ones. A full resynchronization snapshot identifies its sequence boundary, after which incremental events apply. If a bounded event queue drops data, mark a gap and request resynchronization rather than silently pretending completeness.

Deduplicate envelopes by `(origin_octopus_id, origin_boot_id, message_id)` in a bounded per-link window. Each operation requires a canonical nonzero decimal `command_sequence` within the current prepared plan; there is no missing/zero legacy bypass. Deduplicate operation outcomes by `CommandId` within that plan/session and retain them for a bounded window. A retained identical duplicate returns its recorded result; after eviction the sequence high-water mark rejects replay instead of performing the action again. Reject duplicates outside a known valid plan instead of executing them again. Version/schema work must define cache capacities and expiry consistently with maximum outstanding operations.

Results distinguish `RECEIVED`, `REJECTED`, `APPLIED`, `OBSERVED`, `FAILED` and `UNKNOWN_OUTCOME`. `RECEIVED` means accepted into bounded execution state. `APPLIED` identifies the precise local wire milestone. `OBSERVED` attaches equipment evidence and age; it is not fabricated because a send succeeded.

A lost result may be queried on the current synchronized link. A process crash after a CEC action but before recording the outcome cannot generally prove whether the appliance acted. Do not promise exactly-once delivery. Discrete desired state can be reconciled with evidence; relative volume, toggle, navigation and repeated key presses are not replayed automatically after uncertainty or reconnect.

## 9. Backpressure and diagnostics

Maintain separate bounded application priorities: emergency cancellation/release, plan/control, state observations, bulk transfer and trace. Reserve capacity for cleanup. Limit each peer's outstanding operations and transfers, and apply a global coordinator budget so one member cannot consume all heap.

A priority queue cannot bypass bytes already handed to TLS/TCP. Keep writes bounded, avoid queuing large traces in socket buffers, and rely on the local authority/liveness monitor when the network stalls. A slow peer must not block dispatch to other members or timing on the coordinator's own tentacles.

Reserve transport admission for exact held-key cleanup and prioritize it over unsent ordinary traffic. If a current active hold's release cannot be admitted at either hop, retire/shut down that exact authenticated link so owner link-loss fencing cancels repetition. Keep its cleanup context until successful admission or link fencing. A missing/duplicate/stale token is harmless cleanup and must not close a current link or indirectly cancel a newer hold. Command-result ACK/NACK/timeout/unknown reports do not change physical button state: the controller retains the exact up/cancel context until the matching transition or explicit context fence, even when a wire attempt reports failure.

At the emitting executor, failed key TX ends that key's pressed state. A later exact-token up is therefore a no-op even if transport still retains its correlation: it must not become an overload error that closes a healthy link and cancels a newer key. Confirm inactivity from exact current owner state rather than guessing from a transport timeout or an expired operation-result cache. The network up remains a state transition; only the executor derives the recipient's implicit global CEC release when all its keys cease being pressed. A failure result never causes the controller to synthesize another button transition.

Report queue depth, oldest age, dropped/coalesced observations, connection state, negotiated version, heap low-water mark and last protocol error. Logs may include qualified IDs and opcodes, but not setup codes, Wi-Fi credentials, private keys, SRP material or bearer credentials.

Canonical error codes include `UNAUTHORIZED_ROLE`, `WRONG_CONSORTIUM`, `STALE_BOOT`, `STALE_EPOCH`, `STALE_CONFIG`, `STALE_BINDING`, `STALE_DOMAIN`, `STALE_ROUTE`, `LEASE_EXPIRED`, `TARGET_NOT_LOCAL`, `NOT_READY`, `RESOURCE_CONFLICT`, `QUEUE_FULL`, `UNSUPPORTED_CAPABILITY`, `PROTOCOL_MISMATCH`, `INVALID_MESSAGE` and `UNKNOWN_OUTCOME`. Include correlation IDs and actionable status, never a guessed successful result.

## 10. Implementation and tests

Build transport codecs and a simulated channel before coupling to TLS. Then integrate pinned TLS and the real event loop without modifying CEC ISR behavior. Test fragmented lengths/bodies, oversized frames, duplicate keys, uint64 overflow, unknown fields, slow readers, abrupt disconnect, reordering across reconnect, replayed boots and duplicate node identity.

Inject delay longer than an ingress or execution lease: the old input/command must fail even with a seemingly fresh `ttl_ms`. Test lease rollover, at most two live IDs per class, rejected controller handover/promotion, target mismatch, stale bindings and the same LA on several tentacles. Saturate trace/config transfer while holding a key; confirm local release remains bounded and other members remain responsive.

The measured socket/RAM budget must include existing HTTPS, Matter, HomeKit, BLE and all CEC engines. The target of eight total octopuses means seven member control links at the coordinator, in addition to other services. If that target does not fit with the required margin, reduce the advertised cap or change the resource design; do not disable verification, remove queue bounds or silently evict members to pass a nominal capacity target.

## Per-adapter settings messages

The current capability mask is **61439**, adding `per_adapter_settings` (32768) to the existing negotiated registry. Older peers lacking it remain visibly unsupported for Settings; do not send an unknown message or substitute a web listener. Only the current authenticated original controller can request a settings write, under an exact receiver-issued current control/execution lease and original remaining receiver deadline. Revalidate that authorization immediately before the owner's bounded write callback.

* `settings_read`: closed `{request_id}`; no CEC scan/probe or authority change.
* `settings_write`: closed `{request_id, execution_lease_id, settings}`, with settings exactly the [HTTP write core](12-control-surfaces-and-admin.md#per-device-settings-contract) after removing the outer request ID: `{expected_boot_id, expected_settings_revision, confirm_restart:true, changes}`. The outer request ID correlates the result; changes contain 1–5 unique slot/presence-generation edits. Queuing never extends its deadline.
* `settings_result`: closed `{request_id, operation, stage, error, snapshot, rebooting}`. Operation is `read|write`; stage is `admitted|applied|failed`. Admitted is write-only with empty error, null snapshot and rebooting false. Applied has empty error and a valid current snapshot; read uses rebooting false and durable write uses true. Failed has a closed nonempty error, null snapshot and rebooting false. Errors are `INVALID_MESSAGE`, `STALE_BOOT`, `CONFLICT`, `NOT_READY`, `LEASE_EXPIRED`, `STORAGE_FAILURE`, `QUEUE_FULL`.

Snapshots carry exact boot, settings revision and at most five unique stable slot records as specified in document 12. A result does not authorize replay. The owner schedules only the explicitly selected device's restart after durable success and the applied-response transmission attempt, even if transport delivery fails. A disconnected client reports uncertainty and performs fresh reconciliation; it never retries a write automatically. Main/member roles and sole-controller authority remain unchanged.
