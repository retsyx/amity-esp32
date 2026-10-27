# 16 — Agent work breakdown

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

**21 October 2026 algorithm-preservation amendment:** [AGENTS.md](../AGENTS.md) and [the normative preservation rule](21-original-controller-policy.md#original-algorithms-and-functionality-amendment--21-october-2026) apply to every work package. The original algorithms are authoritative unless the user requests a change; consortium ownership/transport layers must preserve behavior. Compare baseline source/tests before implementation and record necessary distributed adaptations and parity evidence.

**Status:** proposed implementation plan. This document gives work packages that implementation agents can divide into reviewable changes. It does not assert that any package is implemented or that a hardware gate has passed.

## 1. Rules for parallel implementation

One agent owns each contract-changing package. Other agents consume its versioned interface and fixtures; they do not fork competing interpretations of qualified addresses, policy generations or enrollment state. Read [01](01-octopus-architecture.md), [02](02-consortium-architecture.md), [03](03-domain-model-and-addressing.md) and the owning implementation guide before changing code.

For user-facing work, [20](20-user-interface.md) owns baseline workflows, vocabulary and device-list presentation and UX-01 through UX-16. [12](12-control-surfaces-and-admin.md) continues to own adapters/APIs/integrations, and [11](11-config-storage-and-migration.md) owns configuration persistence and activation. Do not make a technical work package's internal fields into mandatory setup questions.

Keep the current one-tentacle build operational through the refactor. Introduce interfaces with a local adapter first, then move behavior behind them. Do not create four copies of the legacy hub, replace its queue protections with network callbacks, or begin by writing a distributed election system.

For each task, the implementing agent must state: existing behavior, intended contract change, files/modules touched, dependency version, failure behavior, tests that establish it, and unresolved gates. A test that mirrors an implementation detail is insufficient evidence for source isolation, secure enrollment or timing capacity.

## 2. Work packages

The dependency column denotes required contracts or completed evidence, not necessarily whole release milestones. A package may develop against a stable test double while a dependency's hardware implementation is in progress.

| ID | Work package and primary output | Dependencies | Completion evidence |
|---|---|---|---|
| W00 | Freeze source/SDK baseline and translate this package into tracked tasks. Record any intervening upstream changes. | None | Source manifest and reviewed decision/register delta. |
| W01 | Strong ID types, distinct QualifiedDeviceName and resolved-address types, canonical codecs, revisions and snapshots. | W00 | OSD-name/address substitution rejection, scoped duplicate names, overflow and repeated-address fixtures. |
| W02 | Multiport/multinode deterministic simulation harness with independent clocks, domains, packet faults and power cuts. | W01 | Tests can reproduce stale boot, shared domain and protected-egress absence. |
| W03 | BoardProfile registry, electrical/pin audit and supported button/indicator/USB setup profile. | W00; gesture contract [10](10-ble-and-matter-coexistence.md) | Validated pin mapping; debounce and 3/10-second boundary/continuing-hold tests; one press opens at most one eligible workflow; printed/browser instructions match actual hardware. |
| W04 | Shared CEC timing runtime and independent nonblocking TX scheduling. | W02, W03 | One-to-eight simultaneous-line behavior, timing/IRAM measurements; [G01](17-decisions-and-open-questions.md) closes for advertised capacity. |
| W05 | Passive monitor-all receive path separated from ACK ownership. | W02; integrate W04 | Directed traffic observed without foreign ACK; bounded loss counters. |
| W06 | Domain identity allocator and four explicit port roles. | W01, W04, W05 | Shared-bus collision, occupied TV0, playback exhaustion and inactive-role tests. |
| W07 | Asynchronous per-domain scanner, observation registry, scoped OSD-name resolver and immutable bounded discovery snapshots. | W01, W02, W05 | Automatic LA churn, ambiguity/reuse/alias cases and bounded scans; UX-05/06/10 consume evidence without duplicate equipment cards or repeated scans. |
| W08 | Physical-address evidence resolver, bootstrap dependency ordering and targeted connection questions. | W06, W07; UX contract 20 | Ambiguous/late/child-only reports and dual-output AVR cases; UX-06/07 ask only unresolved physical questions, retaining confirmed answers. |
| W09 | LocalCecAgent, semantic policy compiler, source-isolation egress rules and admission/barrier API. | W01, W04, W06 | No unauthorized egress across startup/scan/config/partition; stale frame-start checks. |
| W10 | SystemControl facade and local GroupService adapter replacing global hub assumptions. | W01, W09 | Existing one-tentacle behaviors run through facade; no standby-on-label-edit regression. |
| W11 | Pure topology/resource planner, validated activity suggestions, sessions and dependency-graph execution. | W01, W02, W08, W09 | Shared receiver/conflict cases, unsupported path rejection and safe reconciliation; UX-07/08/09 suggestions remain non-actuating until explicit intent. |
| W12 | Strict schema2/name migration, origin record, compiled slices, journals, stable external slots and bounded validated candidates. | W01, W10; storage/UX contracts 11/20 | UX-02/04/11 preserve names, overrides and unaffected choices; power-cut/CAS tests and shared 32 KiB pending-payload/storage budget pass. |
| W13 | Operational TLS, bounded transport, handshake, leases, deduplication and atomic runtime binding updates. | W01, W02; authority contracts 09/10 | Pin/role/replay tests, binding acknowledgment and measured budget; a physical setup gesture/window alone never grants an operational session or bypasses membership state. |
| W14 | Single-host BLE integration spike, pre-start GATT hook, RadioArbiter and physical-entry admission. | W00; gesture contract 10 | One press cannot open both setup modes; continuing holds do not enter shorter modes; Matter/enrollment/remote contention is serialized without duplicate host init. |
| W15 | Reviewed Security2 client/server interoperability and enrollment GATT framing. | W14; security review | Exact pinned SRP proof/encryption vectors; malformed/timeout/attempt-limit tests. |
| W16 | Membership/Wi-Fi transaction, opaque nearby relay and LAN activation proof supporting the Amity Add workflow. | W12–W15 | Join journal boundaries survive restart; no-fabric member reconnects without auto commissioning; UX-01/03 reuse authorized information without skipping physical/code/LAN proof. |
| W17 | Consortium routing, compiled-policy synchronization and automatic runtime name-binding synchronization integrating planner and local agents. | W10–W13, W16 | Two nodes with delayed/reordered messages; no actuation before sync or after stale authority. |
| W18 | Sole original-controller remote, stable multi-screen activity mapping and exact-token button state changes; original completion-paced held-key loop runs at the emitting owner. Reject member input and per-button renewals. | W10, W13, W17 | SC-01/02/04/05, V-INPUT-01/02/03 and amended UX-07/11; no owner moves or member gateway. Current-main local/remote parity, immediate independent command/key admission and no queued repeat bursts. |
| W19 | Preserve baseline activity/device UI with canonical Amity-hostname context and one Amity management tab. | W12, W17 | Amended UX-01–UX-16; no wizard/Equipment/Settings or new mapping/primary-zone UI. |
| W20 | Matter/HomeKit primary-zone adapters and alias/selector migration. | W10–W12, W17 | UX-02/13 retain pairings and stable selectors; no implicit group-all-off or cloned identity; added screens do not silently republish the primary zone. |
| W21 | Actuator removal/replacement, manual failed-main backup recovery under fresh authority, and rejection of live transfer/legacy promotion. | W12, W13 | SC-01/02/06/07; no member UI, handover or cloned pairings. |
| W22 | Group OTA orchestration and mixed-version/rollback policy. | W12, W13, W17, W21 | Followers-first compatible update, interrupted download/reboot/rollback, preserved origin. |
| W23 | Production credential storage, signed-update/boot policy and provisioning tooling. | W12–W16, W22 | Security review, recovery drill and approved hardware lifecycle; no ad hoc eFuse changes. |
| W24 | Full HDMI interoperability and capacity qualification. | W03–W09, W17–W22 | Wire captures, actual video/HPD observations, radio/flash load, resource margins at advertised cap. |
| W25 | Documentation, release criteria, migration guide and support diagnostics. | All releasable packages | Capability and UX-01 through UX-16 traceability complete; UX-16 records baseline comparison/accessibility/user results; unresolved trust/hardware gates remain visible. |

Ranges such as W12–W15 mean all listed packages in that inclusive range. They are not permission to serialize all engineering work: interface definitions, simulation, BLE spikes and hardware audit should begin in parallel.

## 3. First implementation tranche

Start W00/W01, W02, W03 and W14 concurrently. These expose the largest architectural risks early: address semantics, test realism, safe GPIO/timing capacity and the shared Matter BLE host. W15 is also an early spike because the repository's HomeKit SRP implementation is not a drop-in Security2 client.

The first useful software milestone is a **one-tentacle octopus through the new facade**, with stable IDs, policy/binding generations and all existing remote/CEC regression guarantees. It should not require a second board or a production provisioning redesign. Keep new consortium APIs unavailable until their trust and membership prerequisites are implemented; a temporary unauthenticated control port is not an acceptable integration shortcut.

The next useful milestone is **several tentacles on one processor**, exercising electrical domains, source isolation, PA bootstrap and shared resource planning without network uncertainty. Qualify the claimed port count rather than assuming the four-tentacle profile is ready because the firmware compiles.

Then demonstrate **two octopuses** with authenticated enrollment, one coordinator, independent clocks, full bindings and stale-work rejection. Larger group sizes are a capacity qualification phase; they are not necessary to establish basic distributed correctness.

## 4. Suggested review boundaries

### Domain and codec changes

Keep strong IDs, serialization and schemas together in one reviewed change. Include negative fixtures. Do not let an integration PR introduce a second definition of address, unknown logical address or revision encoding.

### Driver changes

Separate behavior-preserving engine extraction from timer sharing, and timer sharing from scheduling/queue-size changes. Review IRAM/data placement and link-map evidence with the actual target build. A host test cannot prove GPIO electrical safety or interrupt-service margin.

### Planning changes

Land pure graph/resource decisions before side-effectful execution. Then add plan preparation/barriers and operation outcomes with fault tests. Retire legacy autonomous `source_thief`, implicit re-route-on-discovery and standby-on-config behavior only when the replacement policy has equivalent or deliberately changed tests.

### Enrollment changes

Keep GATT/host coexistence, cryptographic interoperability, Wi-Fi transaction and membership activation as separately reviewable layers. A successful BLE connection is not a successful enrollment. The candidate must prove its operational key on the LAN and durably enter the expected membership state.

Implement document 10's button-equipped profile as one debounced gesture: release below 3 seconds requests eligible first-board Matter setup; release from 3 to below 10 seconds requests eligible consortium setup; reaching 10 seconds takes the factory-reset path without previously opening either shorter mode. State/arbiter checks apply to every request. Qualify supported indicators and retain the USB-assisted physical path for buttonless hardware; a browser button is not an undocumented substitute.

### Integration changes

Migrate external selector IDs and HomeKit aliases before enabling UI reorder. Route all adapters through the facade before publishing multizone internal state. Preserve the initial one-primary-zone contract until additional endpoints/accessories have their own design and tests.

### Baseline presentation

Preserve the referenced original tabs, forms and controls. Device listings add canonical Amity-hostname/adapter context and one Amity tab manages members. Backend scoped selectors, CAS, resource checks and explicit activity execution remain required; no wizard, server setup draft or remote-owner mapping is introduced. Test actual baseline workflows and accessibility.

## 5. Integration milestones

| Milestone | Required visible result | Gate before next milestone |
|---|---|---|
| M0 — Contract baseline | Agent tasks, IDs, envelopes, harness interfaces and document 20 ownership agree. | No competing identity/protocol/UI contracts; UX-01 through UX-16 mapped. |
| M1 — Local compatibility | Legacy one-tentacle system runs through new architecture and retains its existing setup. | Queue/release/config/PA regressions and baseline migration pass. |
| M2 — Local octopus | Several isolated sources/displays under one planner through the original activity fields. | Source prevention, domain/PA correctness, qualified timing and explicit Start/no-hidden-actuation evidence. |
| M3 — Two-node consortium | Authenticated Add, original-controller plans and actuator removal. | Join/crash/partition/hold/trust tests and UX-03 pass; unresolved browser-trust limitations remain visible. |
| M4 — System lifecycle | Actuator replacement, manual failed-main restore, OTA and migration work. | Recovery is executable, identities are not cloned, UX-11/12 preserve valid choices. |
| M5 — Qualified release | Advertised capacity, supported AV configurations and observed normal-user journeys. | [Release evidence](15-validation-and-release-gates.md), applicable UX-01 through UX-16, security/product gates and user documentation complete. |

## 6. Handoff format for subtask agents

Every subtask should name the owning document, exact invariant it implements, relevant source baseline, expected API/schema changes, test fixture, dependencies and exclusions. Example:

> Implement monitor-all delivery in the CEC engine while retaining separate ACK ownership. Owner: document 04/06, W05. A directed frame for another logical address must be observable but never acknowledged by this engine. Preserve normal addressed and broadcast receive behavior. Add deterministic waveform tests and counters; do not implement topology merging or raw forwarding in this task.

When evidence invalidates a proposed decision, record the result and amend the owner document rather than hiding the change in code. Useful negative findings include a four-tentacle GPIO profile that is unsafe, a receiver that rejects extra playback identities, or a BLE API that cannot coordinate commissioning without a narrow pinned-SDK patch. Those results should reduce supported scope or trigger an explicit design change.

## 7. Definition of done

An implementation package is done when code, contract, tests and user-visible status agree; negative paths have bounded behavior; dependencies use the same protocol/model version; and the source/validation record states what was actually tested. A release is done only when [requirements](18-requirements-and-traceability.md) have evidence for the advertised capabilities. Passing compilation, a single happy-path activity or a mocked browser test is not sufficient for group correctness.
