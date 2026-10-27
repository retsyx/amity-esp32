# 03 — Domain model and addressing

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

**Status:** proposed v0.1. This document owns identifier and topology semantics. [Transport](08-consortium-transport-and-routing.md) owns serialization and messages; [configuration](11-config-storage-and-migration.md) owns durable configuration activation.

## 1. Extend name-based resolution with attachment scope

The current `Device` contains a front/back side, logical address, physical address, type, vendor and name; `Activity` resolves display/source/audio/switch by OSD name. Preserve that name-to-current-address behavior. The [current resolver](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp) already replaces an earlier record when a known OSD name moves to a new logical address. Across an octopus or consortium, qualify the configured name by consortium, octopus, tentacle and side instead of using a global OSD-name key. A logical address is transient runtime state and MUST NOT be the persisted device-name suffix. [Current model](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/include/amity/hub.hpp).

The model must separate **who a component is**, **where it was observed**, **which port can control it**, and **which revision authorized that control**.

## 2. Canonical identifiers

All opaque IDs below are random 128-bit values serialized as exactly 32 lowercase hexadecimal characters. Generate with a cryptographically secure RNG when the relevant record is created. All-zero is reserved and invalid. IDs are not derived from MAC addresses, IP addresses, OSD names, Matter Node IDs or array positions.

| Identifier | Scope and lifetime |
|---|---|
| `OctopusId` | Permanent device origin record, retained through ordinary reset; full reprovisioning rules are explicit. Operational keys may rotate independently. |
| `ConsortiumId` | One system's membership/configuration namespace, including a standalone singleton. Fresh system creation produces a new ID. |
| `BootId` | New on every octopus boot; never restored from persistent configuration. |
| `ComponentId` | Internal reference to a configured component record containing qualified OSD names; retained across verified recabling or replacement. It is not a CEC hardware serial number or a substitute for name resolution. |
| `DomainId` | A configured electrical CEC domain; merge/split operations create explicit lineage and revised membership. |
| `BindingId` | One explicit control binding or proxy representation of a component. |
| `ActivityId`, `ZoneId`, `ResourceId`, `RouteId` | Stable desired-state objects and validated paths independent of UI order and labels. |
| `SessionId`, `PlanId`, `CommandId`, `MessageId`, `ScanId`, `ResolutionId` | One runtime operation/session or binding-resolution exchange; IDs are not reused after a crash. |

`TentacleId` is an integer **0–3**, assigned by the hardware profile to a stable connector slot. It does not change when the tentacle is unplugged or renamed. The UI displays HDMI 1–4; firmware and wire contracts use zero-based slot IDs. `side` is the literal string `front` or `back`. `logical_address` is integer 0–14 for a component endpoint. Address 15 is reserved for broadcast/unregistered protocol context and is not an equipment identity.

Every revision or ordered counter whose C++ type is `uint64_t` is serialized to JSON as a canonical decimal string without leading zeros, except `"0"`. Examples: `"0"`, `"19"`, `"18446744073709551615"`. The parser must reject overflow. Do not allow JavaScript's numeric precision to corrupt revisions. Small bounded quantities such as tentacle index, opcode, millisecond limits and protocol version remain JSON numbers.

## 3. Qualified device names and resolved addresses

### Configured device name

`QualifiedDeviceName` is the persisted discovery selector. Its final field is the OSD name, never a logical or physical address:

```json
{
  "consortium_id": "10000000000000000000000000000001",
  "octopus_id": "20000000000000000000000000000001",
  "tentacle_id": 2,
  "side": "back",
  "osd_name": "Apple TV"
}
```

The conceptual spelling is `consortium / octopus / tentacle / side / OSD-name`. The structured object is canonical storage and API syntax; do not parse a human-readable string because an OSD name may contain delimiters. This conceptual spelling does not prescribe a path-like normal UI: [baseline interface](20-user-interface.md) uses an equipment title with the owning Amity device's canonical hostname and adds connector detail only when necessary. Preserve the exact OSD name under the shared CEC decoder; do not case-fold, trim, or replace it with a user-editable label. A missing or malformed OSD response is unresolved evidence, not permission to use a logical address or a fabricated label as the name. Model-specific decoding/fallback quirks require an explicit, tested policy.

All five fields are mandatory. The same OSD name at a different qualified attachment is a different selector. Several selectors may intentionally identify one physical appliance when verified observations establish aliases. Multiple native appliances matching one selector are ambiguous; require a distinguishing name, attachment or explicitly documented matching constraint. A remembered LA is never a durable disambiguator. See [discovery](05-discovery-and-topology.md) for shared-domain correlation and ambiguity handling.

### Resolved CEC address

`FullyQualifiedCecAddress` is a **runtime execution locator** produced by discovery and binding resolution. It is not a configured device name. Its exact object is:

```json
{
  "consortium_id": "10000000000000000000000000000001",
  "octopus_id": "20000000000000000000000000000001",
  "tentacle_id": 2,
  "side": "back",
  "logical_address": 4
}
```

The corresponding diagnostic URI is:

```text
cec://10000000000000000000000000000001/20000000000000000000000000000001/2/back/4
```

Every segment is mandatory; there are no relative URI shortcuts in queued or transmitted commands. This URI describes a resolved runtime address, not a persistent device name or a network URL. Its decimal tentacle and logical-address fields have no leading zeros. Parsers must reject uppercase/noncanonical IDs, unknown sides, path suffixes and out-of-range addresses. Numeric physical addresses are not appended as identity segments.

`CecPortRef` is the shared four-field prefix: consortium, octopus, tentacle and side. It identifies an observation/control attachment point. `QualifiedDeviceName` adds `osd_name`; `FullyQualifiedCecAddress` adds `logical_address`. Their types and schemas MUST remain distinct. A CEC frame separately carries initiator and destination addresses; a broadcast destination is allowed there. Code must not overload a component endpoint to represent a broadcast.

Include `consortium_id` even though a device belongs to one consortium at a time. It prevents stale backups, logs and commands from a previous household membership from becoming meaningful in a new one. An address alone still does not authorize actuation: all binding and generation checks below are required.

## 4. Components, observations and bindings

### Component

A `Component` has `component_id`, user label, `qualified_names: QualifiedDeviceName[]`, equipment kind, configured capabilities and evidence references. Configured CEC components have at least one qualified name; the first is the primary name and further entries are explicitly verified native-attachment aliases. An unresolved/non-CEC placeholder may have an empty list but cannot gain a native CEC execution binding. Offline components retain their configured names even with zero observations. Activities may use `ComponentId` as a normalized reference to this record; their persisted device selection therefore resolves through its qualified OSD names, never a remembered LA.

A component can have several HDMI inputs/outputs and appear on several monitored ports. A qualified OSD name is the configured matching key, but a bare OSD name, vendor ID or type is not a globally unique appliance identifier. A user-facing label can be changed independently of the appliance's reported OSD name. Renaming that label does not rewrite `qualified_names`.

The baseline activity device lists retain their existing behavior with the owning Amity hostname and HDMI-adapter context. ComponentId and full scoped selectors travel behind the unchanged fields; displayed labels, ordering and current LAs never become identity. Proxy representations cannot be offered as native source choices. [UI contract](20-user-interface.md).

Capabilities are three-valued where necessary: supported, unsupported or unknown. Unknown independent receiver outputs, return audio or exact input selection must not be treated as supported. User-confirmed capabilities are distinct from probe results and should state the model/port to which they apply.

### Observation

A `CecObservation` contains qualified endpoint, observer `BootId`, observation sequence, `DomainId` and `domain_revision`, discovered fields, scan association and evidence quality. Use receiver-local age and a stale flag for snapshots. A sender's monotonic timestamp may be retained for diagnostics but cannot be compared to another octopus's clock.

Physical address is a 16-bit CEC field represented as an integer 0–65535 in raw observations. `0xffff` means unknown. Validate the topology form and peer/device context before treating a report as usable. A valid numerical PA does not establish which physical HDMI connector was used or that the reporting device is the immediate neighbor.

### Binding

A `ComponentBinding` grants a particular capability through one local executor:

```text
binding_id, component_id, capability
device_name: QualifiedDeviceName  // native_appliance bindings only
endpoint: FullyQualifiedCecAddress
owner_octopus_id
domain_id, domain_revision
binding_revision
role, physical_address_evidence_ref
status: unresolved | tentative | verified | quarantined | retired
```

An unresolved configured name with no observed endpoint has **no runtime binding yet**. Represent it in the component's resolution status; do not invent LA 0 or 15 to construct a binding. A historical binding may retain its last-known endpoint while marked unresolved/quarantined/retired, but it is non-actionable. Only a currently verified binding with matching policy and freshness checks can execute.

The endpoint's octopus and `owner_octopus_id` must agree. A native binding MUST contain `device_name`, equal to one of its component's configured qualified names, and the name's port prefix MUST equal the endpoint's port prefix. The discovery service must establish an unambiguous current name-to-address resolution before verification. A proxy binding MUST NOT contain `device_name`: its endpoint is a locally allocated representation, and its advertised proxy OSD name is role configuration, not the native appliance's discovery selector. The component may have other observed endpoints; only the selected binding can execute that capability. An AVR may have separate bindings for routing and audio, but a single resource must have one owner. The opcode compiler chooses wire initiator/destination from native/proxy binding kind; it must not transmit from an appliance's address merely because the command names it as target.

A proxy can represent a source on the downstream side while the actual source is addressed on its isolated upstream side. These are two bindings to one component, not proof that their logical or physical addresses are interchangeable. Role-specific operations state whether they target an appliance binding or proxy binding. See [CEC policy](06-cec-roles-and-message-policy.md).

An AVR-output proxy can keep one physical LA/PA while representing different selected receiver sources over time. Alternative source representation bindings MAY therefore share that proxy endpoint. They must refer to one allocated proxy-identity `ResourceId`, with at most one active representation for that domain/path at a time. Allocate the logical address once, not once per source. This exception does not merge the source components or permit concurrent conflicting representations. Native equipment observations at one current domain/LA are one observed identity, not evidence for several independent appliances.

### Binding changes

Rescanning may update an observation without updating a binding. An unambiguous match for the same configured qualified OSD name on its expected attachment MUST automatically update its runtime logical address. For example, `Apple TV` moving from LA 4 to LA 8 preserves `ComponentId`, `qualified_names` and activity references. It requires no user confirmation, configuration edit, or new Select/Resume intent solely because the LA changed. Runtime resolution advances `binding_revision` and any affected domain allocation revision, without changing `config_revision`.

Before activating the new resolution, close admission for the old binding revision and invalidate old queued frames, held-key contexts and affected plan steps. Do not retarget a previously admitted press, hold or release to the new endpoint. Fresh input may use the new resolution after current policy/route checks. Discovery itself emits no power or routing operations; if other evidence leaves the route uncertain, the normal reconciliation policy applies. A component reboot, recabling or a PA change may create additional uncertainty; LA churn alone does not.

Local discovery immediately fences affected old native bindings, then reports the new evidence. The coordinator synchronizes an agreed new resolution with the owning actuator before admitting fresh work; standalone uses the same interface locally. Update all capability bindings derived from the changed native selector together. An offline member may gather evidence but cannot grant itself group execution authority or rewrite queued targets. Runtime binding synchronization is owned by [transport](08-consortium-transport-and-routing.md).

Changing the configured name selector, domain/owner assignment or PA policy is a semantic configuration edit. An ambiguous match, a name observed on an unapproved attachment, or a different appliance taking the former LA MUST NOT silently replace the component. Increment relevant revisions and reconcile affected execution when those changes are approved. Cached endpoints may be retained for diagnostics, but must be revalidated against the configured name and current authority after restart.

Bindings are retired, not recycled for a different component. A replacement appliance may intentionally inherit the logical `ComponentId` after user confirmation; it receives a new or revised binding and old in-flight work is invalidated.

## 5. Electrical domains

A `CecDomain` describes ports electrically connected without an Amity CEC isolation boundary. Domain membership is distinct from video topology. A source tentacle front and an output tentacle back may share the receiver's CEC wire even though they are different cables and octopuses.

The record contains `domain_id`, `domain_revision`, attached port references, evidence/confirmation, `policy_owner_octopus_id`, identity allocations and scanner assignment. Exactly one policy owner authorizes the domain. Several ports may transmit distinct, valid playback identities under that policy; “one policy owner” does not mean “only one physical transmitter.”

One virtual-TV role may be assigned on a source/AVR domain. A real TV already present changes the valid role set. Allocate each logical address once within the domain, including native equipment. Do not let every `back` side independently claim 0 or backup 14. Playback capacity and CEC-version constraints are explicit resources.

Passive correlation can suggest that ports share a bus, but absence of traffic does not prove isolation. Exact duplicate frame/time patterns, controlled scans and user-confirmed wiring contribute evidence. [Discovery](05-discovery-and-topology.md) defines the process. When uncertainty could produce duplicate active identities, quarantine the conflicting role rather than relying on best guesses.

Merging or splitting domains invalidates policy and binding revisions before new commands execute. V1 creates a fresh DomainId for a merged domain and fresh DomainIds for each result of a split, retires the old IDs, and records lineage for diagnostics. Ordinary evidence updates or attachment changes that do not change electrical-domain identity increment the existing domain revision. Every affected binding becomes stale until revalidated; retired IDs are never routable aliases. The coordinator journals newly allocated IDs before activating the transaction so restart cannot allocate a different set halfway through a merge.

## 6. Cable graph and resources

The graph distinguishes four relationships:

| Relationship | Meaning |
|---|---|
| Physical HDMI link | A real source/output-to-sink/input cable, optionally interrupted by a named tentacle. |
| Internal equipment path | A verified receiver/switch mapping from an input to an output. |
| Electrical CEC domain membership | Which CEC ports see the same physical wire. |
| Control binding | Which local CEC identity/role can perform a specified action. |

Do not infer one relationship merely from another. DDC evidence may identify a physical tree while CEC pin wiring is discontinuous or silently shared. Two outputs do not imply two independent switch resources.

A `Resource` declares its owner, supported modes and compatibility rule. Examples include receiver input selector, receiver main audio, output-enable set, display input selector, source transport and local CEC proxy identity. A mode shared by two sessions may be compatible only when values agree; for example two display sessions can share the same receiver input, but cannot simultaneously demand different inputs from a single selector.

An `Activity` names a zone, source, display, audio target, desired physical route and resource constraints by stable IDs. Its component IDs reference the configured qualified-name records above, not stored execution addresses. A `Session` is one runtime activation of an activity. UI order and external input selectors are independent mappings, preserving existing automations after reordering. [Planning](07-activity-planning-and-execution.md), [configuration migration](11-config-storage-and-migration.md).

## 7. Revision hierarchy and stale-work checks

| Token | Changes when | Prevents |
|---|---|---|
| `ConsortiumId` | System is recreated or device joins another system. | Cross-system replay/confusion. |
| Coordinator epoch | Authorized coordinator transition. | Commands from superseded authority. |
| `BootId` | Any sender/recipient restarts. | Reuse of pre-reboot runtime sessions. |
| Configuration revision | Committed topology/activities/policies change. | Execution against a different desired model. |
| Presentation revision | User labels or UI ordering change. | Lost concurrent cosmetic edits; it does not fence CEC work. |
| Domain revision | Electrical membership or identity allocation changes. | Address collisions and stale domain assumptions. |
| Binding revision | Component control endpoint/evidence changes. | Address reuse or recabling redirects. |
| Route generation | Session route changes or is canceled. | Delayed input or plan work affects a newer route. |

Compare these tokens within their exact scope. A higher revision in another consortium is meaningless. IDs and revision matching do not replace cryptographic authentication. An actuator checks the applicable tuple at admission **and immediately before beginning a CEC frame**. A frame already on the wire completes or aborts according to local timing rules; transition barriers wait for that bounded outcome before granting conflicting work.

`config_revision` is the semantic control revision. A pure component/activity-label or UI-order edit increments only `presentation_revision`; both are canonical uint64 strings. Changes to configured qualified names, routes, binding selection/policy, capabilities, actual proxy identity fields, activity membership, integration slots or quirks are semantic. Automatic resolution of an unchanged selector to a new LA is runtime binding state, not a semantic edit. Mixed edits use the semantic activation protocol. Administration compares the expected pair of revisions; storage markers cover both and the complete digest. Presentation-only edits do not invalidate leases, holds or route generations. [Document 11](11-config-storage-and-migration.md) defines revision rebasing for unchanged owners during a semantic change.

## 8. Suggested APIs and ownership

The domain-model library should be host-testable and independent of ESP-IDF. Prefer strong C++ wrapper types over exchanging untyped strings everywhere. Parsing occurs once at the edge; internal IDs are sixteen bytes, not heap-allocated text. Wire serialization uses the canonical representation.

Core contracts include distinct qualified-name and resolved-address codecs, comparing scoped revisions, validating bindings, resolving `ComponentId`/capability through configured `QualifiedDeviceName` to a current execution target, validating resource compatibility, and building immutable system snapshots. A status snapshot may show unresolved components; an execution plan may not contain an unresolved required binding.

Unknown enum values are a protocol-version error unless explicitly negotiated as an ignorable extension. Do not silently map an unknown `side` to front, a missing tentacle to zero, or a missing consortium to the current one. Strict deserialization is especially important during v1 name-based migration.

## 9. Acceptance criteria

- Repeated LA 4 and PA 1.0.0.0 on separate isolated domains remain distinct.
- Two observations of one AVR can be unified without creating two owners of its input selector.
- Two identical OSD names remain separate until explicit evidence or user action joins them.
- A unique qualified OSD name moving from LA 4 to LA 8 preserves configured names, ComponentId and activity references, refreshes its binding automatically, and fences work admitted against the old revision.
- A different appliance taking LA 4 cannot receive the first appliance's queued or newly resolved commands.
- Qualified-name schemas reject logical-address substitution, and runtime address schemas reject OSD-name substitution.
- Same-name matches at the same attachment remain ambiguous unless a documented distinguishing constraint exists; missing OSD evidence never becomes a synthetic LA-based name.
- UI reorder, renaming and IP/DHCP changes do not change ComponentId, ActivityId or external selector meanings.
- A command with any stale binding/domain/configuration/boot/epoch token is rejected before wire start.
- Recabling a source into another tentacle does not retarget a queued key or activation command.
- Canonical serialization round-trips all valid addresses and rejects malformed/out-of-range values, including uint64 overflow.
- Factory reset retains only the intended origin identity/bootstrap record, changes operational credentials and cannot silently resume its previous group authority.

The foundational JSON schemas and examples accompany this package. They are a contract seed; the host domain library and strict firmware parser must also validate relationships that JSON Schema cannot prove, such as ownership, domain membership and cross-object uniqueness.
