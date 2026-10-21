# Amity Octopus and Consortium

## Architecture and implementation specification

**Status:** proposed design v0.1, 20 October 2026. **Audience:** agents decomposing and implementing the work. This package specifies changes; it does not describe features already implemented or claim hardware validation.

**Implementation follow-up:** [Remaining implementation work](TODO.md) tracks the software and release-process gaps found in the 0.1.60 audit. See the [implementation status](../IMPLEMENTATION_STATUS.json) for the current profile, evidence and physical qualification limits.

**Current behavior source:** `main` (`98e64a6` for this merge); distribution only transports messages and has zero CEC behavioral effect. [Normative amendment](21-original-controller-policy.md).

**Historical reference implementation:** [`retsyx/amity-esp32` at `d50af8026938797fc0820d0107f4c4e064c2d2bc`](https://github.com/retsyx/amity-esp32/tree/d50af8026938797fc0820d0107f4c4e064c2d2bc). Requirements originate in [The Octopus, issue 1](https://github.com/retsyx/amity/issues/1), [The Consortium, issue 2](https://github.com/retsyx/amity/issues/2), and the requested TV/projector/shared-receiver scenario. See [source baseline](19-source-baseline.md) for the evidence and SDK pins.

**Algorithm-preservation amendment, 21 October 2026:** original Amity algorithms and functionality are authoritative unless the user explicitly requests a change. The consortium adds an ownership and transport layer around them and preserves behavior for local and remote equipment. This requirement supersedes conflicting older proposals, including per-button hold leases/renewals: communicate button state changes and run the original completion-paced CEC repeat loop at the emitting owner. See the [normative rule](21-original-controller-policy.md#original-algorithms-and-functionality-amendment--21-october-2026) and repository-wide [AGENTS.md](../AGENTS.md).

**Explicit executor/CEC amendment, 21 October 2026:** the executor owns button state and message choices, with no outgoing buffer. CEC only sends supplied messages from its per-wire FIFO with eight normal slots plus one release-only overflow slot; it does not choose key, repeat or release policy. Every key-press frame has one wire attempt. A failed key TX ends that key's pressed state and repetition; later button-up is a no-op and unchanged down/authority renewal cannot restart it. Release is implicit at the end of the receiving device's press stream, including last-key failure, only when all its keys are no longer pressed. Remote key-up updates state rather than directly requesting a key-specific release. These are explicit exceptions to the original retry/failure details. [Normative boundary](21-original-controller-policy.md#executor-and-cec-boundary-amendment--21-october-2026).

**OSD-name amendment, 20 October 2026:** configured fully qualified device names use `consortium / octopus / tentacle / side / OSD-name`. Discovery resolves those names to current logical addresses. Ordinary unambiguous LA changes preserve component/activity references and require no configuration edit or user confirmation. Runtime endpoints remain versioned and stale work is fenced. This amendment is incorporated in the architecture, discovery, migration, transport, schemas, examples and acceptance requirements below.

**UI amendment,20 October 2026:** [document 20](20-user-interface.md) now requires exact baseline UI parity: original Activities/Remote/Matter/HomeKit/Advanced tabs and activity/AVR/audio/switch workflows. Baseline additions are device-list canonical Amity-hostname/adapter context and one extra Amity management tab; the later approved settings and navigation amendments apply as specified below. The prior guided-wizard/Equipment/Settings and multi-member-remote proposals are superseded, not current backlog. The approved 21 October 2026 navigation order is **Activities, Remote, Matter, HomeKit, Amity devices, Advanced**, with Advanced last; this narrow reordering preserves tab contents and the separately approved per-adapter settings exception.

**Original-controller amendment,20 October 2026:** [document 21](21-original-controller-policy.md) fixes the first-setup Amity as the sole remote/UI/admin/integration/planning authority. Added devices are UI-less actuators with no promotion/handover; multiple screens/zones remain supported.

## Recommendation

Build one common control system that scales from a single legacy board to several octopuses. An **octopus** owns one to five **tentacles**; each tentacle is an HDMI pass-through whose two CEC wires are electrically isolated and independently controlled. A **consortium** coordinates octopuses over the home LAN. Video continues to use the physical HDMI wiring.

Use the immutable original first-setup controller for desired state, topology, membership and activity planning, the sole remote and all UI/home integrations. Keep CEC timing, address ownership, source-isolation policy, admission checks and held-key release local to each octopus. The initial consortium is an application-level mesh with a coordinator-routed control protocol; it does not require a Wi-Fi mesh network, cloud service, MQTT broker or separate computer.

The original controller is fixed. Added devices cannot become UI/input gateways or planners through timeout, explicit promotion or planned handover. Controller loss stops new user control while actuator isolation and cleanup continue. Automatic failover/live transfer is outside scope. If the main fails, supported manual recovery uses fresh Matter setup on replacement hardware and the existing Advanced configuration restore, with new pairings and member re-enrollment.

## Reading map

| Document | Purpose |
|---|---|
| [01 — Octopus architecture](01-octopus-architecture.md) | Local hardware/software architecture, roles and system boundaries. |
| [02 — Consortium architecture](02-consortium-architecture.md) | Distributed architecture, end-user lifecycle and worked system topology. |
| [03 — Domain model and addressing](03-domain-model-and-addressing.md) | Qualified OSD names, resolved runtime addresses, stable references, domains and bindings. |
| [04 — Hardware and CEC runtime](04-hardware-and-cec-runtime.md) | Ten-line driver, shared timer, board profiles, timing and memory gates. |
| [05 — Discovery and topology](05-discovery-and-topology.md) | Asynchronous scanning, physical-address evidence, domain correlation and recabling. |
| [06 — CEC roles and message policy](06-cec-roles-and-message-policy.md) | Proxy identities, source isolation, translation and egress rules. |
| [07 — Activity planning and execution](07-activity-planning-and-execution.md) | Multi-zone resource arbitration, distributed execution and remote input. |
| [08 — Consortium transport and routing](08-consortium-transport-and-routing.md) | Connections, envelopes, leases, ordering, acknowledgements and backpressure. |
| [09 — Membership, security and enrollment](09-membership-security-and-enrollment.md) | Secure addition, credentials, operational trust and authorization. |
| [10 — BLE and Matter coexistence](10-ble-and-matter-coexistence.md) | One NimBLE host, GATT integration, workflow arbiter and Wi-Fi ownership. |
| [11 — Configuration, storage and migration](11-config-storage-and-migration.md) | Schema migration, persistence, stable external selectors and crash consistency. |
| [12 — Control surfaces and administration](12-control-surfaces-and-admin.md) | Control adapters, administration APIs, Matter, HomeKit and remote ownership. |
| [13 — Faults, removal and recovery](13-faults-removal-and-recovery.md) | Offline behavior, actuator removal/replacement and original-controller recovery. |
| [14 — OTA and compatibility](14-ota-and-compatibility.md) | Version negotiation, staged rollout, rollback and firmware trust. |
| [15 — Validation and release gates](15-validation-and-release-gates.md) | Simulation, fault injection, hardware qualification and release evidence. |
| [16 — Agent work breakdown](16-agent-work-breakdown.md) | Implementable work packages, dependencies and review boundaries. |
| [17 — Decisions and open questions](17-decisions-and-open-questions.md) | Adopted decisions and explicitly unresolved engineering gates. |
| [18 — Requirements and traceability](18-requirements-and-traceability.md) | Requirements mapped to design ownership and acceptance evidence. |
| [19 — Source baseline](19-source-baseline.md) | Existing behavior, exact source references and limits of this review. |
| [20 — Baseline user interface](20-user-interface.md) | Original tabs/forms, canonical Amity-hostname context, one Amity tab and amended UX-01 through UX-16. |
| [21 — Original controller and subordinate devices](21-original-controller-policy.md) | Dated fixed-controller, one-remote, no-member-UI amendment, migration and SC-01 through SC-08. |

The `schemas/` directory contains machine-readable contracts for foundational identifiers and protocol envelopes. The `examples/` directory contains fixtures illustrating those contracts and the TV/projector scenario. These are **proposed interoperability fixtures**, not deployable firmware configuration. Each schema states its coverage; the full portable configuration and all opcode policies must not be inferred from an illustrative example.

## Instructions for implementing agents

Read [AGENTS.md](../AGENTS.md) first. Compare relevant baseline algorithms and tests before changing existing behavior; record necessary distributed adaptations and parity evidence. An older proposal or test is not authorization to change original functionality.

The preservation rule explicitly includes Siri remote swipes: retain the original gesture recognition, timing and generated press/release sequences. The consortium transports those resulting events to their owner without replacing the gesture algorithm.

1. Read documents 01–03 before decomposing a feature. Read the implementation document that owns the feature and its linked dependencies. User-facing work also follows [20](20-user-interface.md); document 12 owns adapters/APIs, while 20 owns the normal journey and presentation.
2. Consult [the decision register](17-decisions-and-open-questions.md). A hardware or SDK gate marked unresolved is a prerequisite requiring evidence, not permission to assume support.
3. Preserve the public contracts in documents 03 and 08. Persist `QualifiedDeviceName` selectors with OSD names; resolve them to current CEC addresses. Internal stable IDs reference those configured records. Do not substitute bare names, IP addresses, array positions or CEC logical addresses for the scoped contracts.
4. Use [work packages](16-agent-work-breakdown.md) to establish ownership and integration order. Add tests at the layer that can actually establish the claimed behavior.
5. Record deviations as amendments to the owning document and the decision register before changing interfaces in parallel branches. Update relevant fixtures and traceability rows with the amendment.
6. Map user-facing implementation evidence to UX-01 through UX-16. Measure novice completion, actual questions and assistance needed; passing a mocked UI or exposing every internal field does not establish minimal setup.

Normative **MUST**, **MUST NOT**, **SHOULD** and **MAY** describe obligations within this proposed design. They are not claims that the current repository meets them. Where source code and this package differ, the source baseline describes today and the proposed contract describes the intended change. Unresolved alternatives are explicitly labeled **gate**.

## Invariants that cross every workstream

- Original Amity algorithms and functionality remain unchanged except for explicit user amendments. Device ownership and transport must preserve their behavior at the emitting owner; network layering does not redefine remote or CEC semantics.
- A tentacle has **two** independent CEC ports. A configured device name includes consortium, octopus, tentacle, side and OSD name; its resolved runtime endpoint uses the same prefix plus current logical address.
- A configured component record has qualified OSD names and an internal stable `ComponentId`; each observation and control binding is separately scoped and versioned. Automatic LA re-resolution does not rewrite activities or persisted names.
- Baseline equipment choices retain their original fields with the owning Amity device's canonical hostname and stable HDMI-adapter context; proxies are not source choices. Component/activity labels never rewrite OSD selectors. Amity-device names equal actual hostnames, with no independent location alias.
- Multiple ports can be attached to the same electrical CEC domain. Address allocation and policy ownership belong to that domain.
- Source isolation is enforced before unauthorized CEC traffic reaches the switching device, even when the network or coordinator is unavailable.
- Every actuator checks current authority, boot, configuration, binding, domain and route generation before starting a frame. An already-started frame completes safely.
- Configuration commit, command receipt, command execution and observed equipment state are different events.
- Discovery and saving activity edits do not wake or route equipment. The original explicit **Start** action is a separate planner intent against committed configuration and current resource/binding checks.
- No distributed “exactly once” claim is made for CEC toggles or relative key actions after an uncertain crash.
- Matter fabrics, HomeKit pairings, the sole controller BLE remote bond, administration credentials and consortium membership are separate security domains. Added devices host none of these user control surfaces; adoption preserves but disables pre-existing unrelated bonds/pairings.
- Offline membership is retained. Removal is explicit. Neither coordinator loss nor removal causes a member to start independently controlling shared equipment.
- A five-tentacle hardware promise and a consortium-size promise require measured timing, memory, addressing and interoperability evidence.

## Initial scope

The initial qualification target is up to **five tentacles per supported octopus profile** and **eight octopuses per consortium**. Eight is a proposed test target, not an architectural limit or a requirement from the issues. A product build must advertise the smaller capacity it has actually qualified. Existing single-tentacle boards remain a supported special case.

Multi-zone control is part of the internal model. The initial Matter/HomeKit presentation remains the existing primary zone; publishing additional zones requires separately qualified endpoints/accessories. ARC/eARC, TV applications, arbitrary HDMI matrix behavior, direct peer actuation and Wi-Fi radio mesh formation are separate capabilities. Controller election, promotion, planned transfer and automatic failover are outside scope and not planned work: headless members preserve isolation, release/expire held work and wait for the original main to return.

Physical setup gestures and their state eligibility follow [10](10-ble-and-matter-coexistence.md), including the explicit USB-assisted path for buttonless hardware. Local browser certificate trust remains a product/UX gate in [17](17-decisions-and-open-questions.md) and [20](20-user-interface.md); convenient labels, mDNS discovery and consortium enrollment do not make the existing HTTPS warning disappear.

No firmware was changed, built or exercised on hardware while preparing this package. Document consistency and fixture validation are recorded in `VALIDATION.md`; the hardware and integration tests described here are work to perform during implementation.

The 20 October 2026 per-adapter settings amendment explicitly moves the former global Advanced default front PA into named devices' **Settings** dialogs. Stable **HDMI adapter N** rows within the hardware profile's capacity distinguish saved default, fresh detected address and selected unconfirmed fallback. Only positively present configured adapters can be saved, with owner boot/revision/presence-generation checks and explicit selected-device restart. This narrow UI exception leaves the original activity forms intact. See the [acceptance matrix](20-user-interface.md#per-device-hdmi-adapter-settings-amendment--20-october-2026), [HTTP contract](12-control-surfaces-and-admin.md#per-device-settings-contract), and [current software task](TODO.md#per-device-hdmi-adapter-preferences).
