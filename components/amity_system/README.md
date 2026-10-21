> 20 October 2026 scope amendment: the original first-setup Amity alone owns the sole remote, UI/admin and home integrations. Added devices are UI-less actuators, with no member input gateway or coordinator promotion/handover. Preserve the original activity UI with only canonical Amity-hostname context and one Amity management tab. [Normative contract](../../amity-octopus-consortium/21-original-controller-policy.md).

# System domain and control core

This component implements the host-testable contracts in design documents 03,
05–07. It has no ESP-IDF, GPIO, wall-clock, JSON-library, or networking dependency.
It uses C++17 and the contract's initial protocol version 1. The development
firmware connects this core to live schema-2 execution through `SystemControl`
and `LiveSystem`. The legacy Hub remains for compatibility and bounded migration
discovery.

`model.hpp` defines separate sixteen-byte strong IDs, the persisted exact
`QualifiedDeviceName`, runtime `FullyQualifiedCecAddress`, structural binding
validation, canonical decimal uint64 codecs, strict flat JSON edge codecs and
canonical diagnostic CEC URIs. IDs must come from a cryptographically secure
RNG supplied by the origin/storage adapter. No implicit name/address conversion,
missing scope, LA 15 endpoint, duplicate JSON property or integer overflow is
accepted. The OSD octet budget is fourteen; labels do not rewrite OSD selectors.

`discovery.hpp` provides bounded, coalescing domain scan jobs and immutable
snapshot copies. A caller advances one probe at a time and supplies validated
observations using its own monotonic clock. A complete scan is required to prove
native-name uniqueness. Old scans, proxy names, stale observations, conflicting
domain/LA reports and unresolved attachment scopes do not become native matches.
Automatic LA refresh first fences every derived old binding and proposes
tentative replacements. The coordinator and actuator must synchronize the
complete replacement set before making it verified. Discovery never emits power,
routing or Active Source actions. Physical-address evidence is attachment and
domain specific; a legacy minimum remains an unconfirmed hint. Identity allocation
uses two vacant wire polls, one TV0, standard playback addresses 4/8/11 and only
explicitly qualified CEC 2.0 backups. Allocation polls are not identity claims.

`policy.hpp` implements `LocalCecAgent`. Installing policy has no CEC side effect.
Semantic actions require a verified binding, local owner/boot, exact epoch,
configuration/domain/binding/policy revisions and current route authority. Every
message decision is validated before direct submission to the CEC layer. The
executor does not buffer outgoing frames; CEC owns the only TX FIFO per wire,
with eight total queued-plus-active messages, including releases. Full
admission returns busy rather than creating an executor backlog. Every TX
dropped because of full-FIFO admission is logged, including each frame of a
rejected explicitly submitted atomic batch. Finite key recipes preserve main:
submit the press directly, then attempt final release immediately only if the
press was admitted and no other key keeps that recipient's stream open. At seven
occupied slots the press enters slot eight and the release drops with a log; at
eight the press drops with a log and no release is attempted. The adapter supplies frame-start guards and
invalidates them when policy or bindings are fenced. Native appliance targets never
supply the initiator identity. Incoming source traffic cannot be forwarded or
authorize power/routing. Identity replies stay local; automatic Active Source
replies require the existing route grant and participate in its barrier.

The executor owns button state, exact targets/tokens and all choices of messages.
It repeats a held press after successful completion when no command is pending,
using the original completion pacing. Devices exchange down/up changes only;
there are no per-button renewals. Explicit user changes require one wire attempt
per key-press frame. A failed key TX ends that key's pressed state and repetition; later button-up
is a no-op, and unchanged down/authority renewal cannot restart it. Release is
implicit at the receiving device's stream end, including last-key failure, only
when all keys for that physical wire/initiator/destination are no longer pressed.
Remote-up updates key state rather than directly selecting a release frame.
End-of-stream cleanup is separate from ended key tokens until its release
can be ordered behind admitted presses.
If the eight-message FIFO is full, drop and log the release and consume the
decision without a later capacity retry or unsent frame queue. Audio query
due state retains the original release-admission +500 ms timing. Address reuse
withdraws unsafe old output permission and records uncertainty. Current
authority/binding/route fences remain mandatory.

CEC is a mechanical transmitter: queue exactly the supplied frames, apply the
supplied attempt budgets and generic frame-start guards, perform the wire
protocol, and report results. It does not interpret key opcodes to select retry,
repeat or release policy, decide button state, or create semantic messages.
Only the executor makes those decisions. Bounded completion correlations for
already admitted work are bookkeeping, not a second transmit buffer.

`planner.hpp` validates actual HDMI paths, verified bindings, owner readiness,
explicit capability/resource modes, input-control ownership and compatible
session demands before reserving anything. Suggestions are non-actuating.
Reservations are atomic, duplicate request IDs retain one result, and stopping
one consumer preserves shared power demands. Preparation and fence/drain
acknowledgments precede dependency-graph execution. Applied steps are distinct
from observed state. Restart suspends desired sessions and clears executable
demands. Output changes with unqualified PA stability are rejected with an
explicit rediscovery requirement rather than executing a partial stale route.

All public state containers have finite limits: ten local ports/jobs, 128
observations per scan, no executor transmit queue, sixteen held-button contexts,
64 policy bindings/components/grants per port, sixteen native names
per component and 32 paths per grant. Fence records coalesce by ID, are reclaimed
only after policy and live work stop referring to them, and fail closed at 640.
The planner accepts at most 64 components/routes/activities, 128 bindings/resources,
256 signal edges, 32 nodes/demands/bindings per route and 64 plan steps. Nested
strings and dependency lists are bounded as well. These are defensive software
limits, not a hardware capacity claim. Persistent configuration limits new
canonical mutations to 16 KiB and compatibility reads to 32 KiB. Storage capacity
preflight can reject a mutation below that limit when journal space is insufficient.

Run `sh tests/amity_system_test.sh` for address/overflow/ambiguity fixtures,
immutable bounded scans, LA churn, PA evidence, allocation exhaustion, stale
admission and frame-start rejection, source suppression, exact-token release,
failure-stopped held states, direct CEC FIFO admission, in-flight barriers,
resource sharing and dependency ordering. The
test compiles with warnings as errors and ASan/UBSan. The legacy Hub regression
suite additionally exercises actual `HubService` admission and guarded Bus calls.
The bounded two-node fixture has independent receiver clocks, separate electrical
domains, delayed/lost/reordered semantic packets and power cuts. It captures
protected egress and proves old boot/route work stays fenced while local hold
cleanup continues. Transport authentication/deduplication have separate tests;
this fixture does not emulate the CEC waveform or cryptographic handshake.

The development firmware integrates persisted scoped component migration,
planner-to-local-runtime execution, guarded CEC work, distributed membership/TLS
activation and the baseline activity adapter. `system_sync` connects authenticated owner snapshots
and prepare/fence/drain/activate barriers to this core. Receive-driven source theft
does not trigger automatic route repair. Supported direct-route compilation covers a
confirmed direct source-to-screen path; unknown receiver output profiles,
PA-changing transitions and return audio remain unavailable without the required
equipment evidence and control profile.

The default hardware profile retains one HDMI adapter/two isolated CEC ports.
An opt-in experimental development profile provides five stable adapter indices
with explicitly configured GPIO pairs. Zero or sparse configured pairs are valid;
missing adapters retain their configured identities and unavailable bindings.
Optional presence sensing uses a separate configured input; CEC silence never
establishes physical absence. Unused connectors stay passive until their
connection is explicitly confirmed. This profile does not qualify five attached
adapters or ten physical CEC lines.

The active Amity system remains limited to three devices; a replacement spare
does not increase that limit. Current bench evidence covers one device with two
isolated CEC lines, plus an independent passive Pi enrollment/TLS peer. It does
not qualify native multi-device enrollment, peer execution or recovery.
Electrical domain confirmation, combined-load timing and memory margins,
real AV equipment held-button/link failures, native ESP32 protocol interoperability,
receiver output capabilities, five-adapter hardware, production security and observed
usability retain the release gates in documents 15 and 17. Host tests and static
IRAM audits cannot close those gates. See the
[implementation status](../../IMPLEMENTATION_STATUS.json) for recorded checks and
remaining qualification requirements.
