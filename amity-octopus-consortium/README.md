# Amity Octopus and Consortium

## Architecture and implementation specification

**Status:** proposed design v0.1, 20 October 2026. **Audience:** agents decomposing and implementing the work. This package specifies changes; it does not describe features already implemented or claim hardware validation.

**Reference implementation:** [`retsyx/amity-esp32` at `d50af8026938797fc0820d0107f4c4e064c2d2bc`](https://github.com/retsyx/amity-esp32/tree/d50af8026938797fc0820d0107f4c4e064c2d2bc). Requirements originate in [The Octopus, issue 1](https://github.com/retsyx/amity/issues/1), [The Consortium, issue 2](https://github.com/retsyx/amity/issues/2), and the requested TV/projector/shared-receiver scenario. See [source baseline](19-source-baseline.md) for the evidence and SDK pins.

**OSD-name amendment, 20 October 2026:** configured fully qualified device names use `consortium / octopus / tentacle / side / OSD-name`. Discovery resolves those names to current logical addresses. Ordinary unambiguous LA changes preserve component/activity references and require no configuration edit or user confirmation. Runtime endpoints remain versioned and stale work is fenced. This amendment is incorporated in the architecture, discovery, migration, transport, schemas, examples and acceptance requirements below.

**Guided-setup amendment, 20 October 2026:** [document 20](20-user-experience-and-guided-setup.md) owns normal user journeys, vocabulary, naming presentation and question order. Users see equipment titles with location context, such as **Apple TV** above **Projector**; the complete qualified selector stays internal. Setup carries forward known choices, supplies stable defaults, asks only unresolved questions and resumes bounded nonsecret drafts. Suggested activities do not execute until the user submits an explicit activity intent. UX-01 through UX-16 define the corresponding acceptance cases.

## Recommendation

Build one common control system that scales from a single legacy board to several octopuses. An **octopus** owns one to five **tentacles**; each tentacle is an HDMI pass-through whose two CEC wires are electrically isolated and independently controlled. A **consortium** coordinates octopuses over the home LAN. Video continues to use the physical HDMI wiring.

Use a designated coordinator for desired state, topology, membership and activity planning. Keep CEC timing, address ownership, source-isolation policy, admission checks and held-key release local to each octopus. The initial consortium is an application-level mesh with a coordinator-routed control protocol; it does not require a Wi-Fi mesh network, cloud service, MQTT broker or separate computer.

The proposal deliberately chooses a stable coordinator in its first version. Automatic failover and direct member-to-member actuation require additional authority and fencing mechanisms and are not implicit consequences of the word “mesh.” A future implementation may add them through a separately reviewed protocol revision.

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
| [13 — Faults, removal and recovery](13-faults-removal-and-recovery.md) | Offline behavior, removal, replacement, coordinator transfer and recovery. |
| [14 — OTA and compatibility](14-ota-and-compatibility.md) | Version negotiation, staged rollout, rollback and firmware trust. |
| [15 — Validation and release gates](15-validation-and-release-gates.md) | Simulation, fault injection, hardware qualification and release evidence. |
| [16 — Agent work breakdown](16-agent-work-breakdown.md) | Implementable work packages, dependencies and review boundaries. |
| [17 — Decisions and open questions](17-decisions-and-open-questions.md) | Adopted decisions and explicitly unresolved engineering gates. |
| [18 — Requirements and traceability](18-requirements-and-traceability.md) | Requirements mapped to design ownership and acceptance evidence. |
| [19 — Source baseline](19-source-baseline.md) | Existing behavior, exact source references and limits of this review. |
| [20 — User experience and guided setup](20-user-experience-and-guided-setup.md) | Normative user journeys, minimal questions, title/location presentation, resumable setup and UX-01 through UX-16. |

The `schemas/` directory contains machine-readable contracts for foundational identifiers and protocol envelopes. The `examples/` directory contains fixtures illustrating those contracts and the TV/projector scenario. These are **proposed interoperability fixtures**, not deployable firmware configuration. Each schema states its coverage; the full portable configuration and all opcode policies must not be inferred from an illustrative example.

## Instructions for implementing agents

1. Read documents 01–03 before decomposing a feature. Read the implementation document that owns the feature and its linked dependencies. User-facing work also follows [20](20-user-experience-and-guided-setup.md); document 12 owns adapters/APIs, while 20 owns the normal journey and presentation.
2. Consult [the decision register](17-decisions-and-open-questions.md). A hardware or SDK gate marked unresolved is a prerequisite requiring evidence, not permission to assume support.
3. Preserve the public contracts in documents 03 and 08. Persist `QualifiedDeviceName` selectors with OSD names; resolve them to current CEC addresses. Internal stable IDs reference those configured records. Do not substitute bare names, IP addresses, array positions or CEC logical addresses for the scoped contracts.
4. Use [work packages](16-agent-work-breakdown.md) to establish ownership and integration order. Add tests at the layer that can actually establish the claimed behavior.
5. Record deviations as amendments to the owning document and the decision register before changing interfaces in parallel branches. Update relevant fixtures and traceability rows with the amendment.
6. Map user-facing implementation evidence to UX-01 through UX-16. Measure novice completion, actual questions and assistance needed; passing a mocked UI or exposing every internal field does not establish minimal setup.

Normative **MUST**, **MUST NOT**, **SHOULD** and **MAY** describe obligations within this proposed design. They are not claims that the current repository meets them. Where source code and this package differ, the source baseline describes today and the proposed contract describes the intended change. Unresolved alternatives are explicitly labeled **gate**.

## Invariants that cross every workstream

- A tentacle has **two** independent CEC ports. A configured device name includes consortium, octopus, tentacle, side and OSD name; its resolved runtime endpoint uses the same prefix plus current logical address.
- A configured component record has qualified OSD names and an internal stable `ComponentId`; each observation and control binding is separately scoped and versioned. Automatic LA re-resolution does not rewrite activities or persisted names.
- Normal equipment choices use title/location context and one card per component; verified aliases expand under Connections, and proxies are not source choices. User labels do not rewrite OSD selectors or a board's hostname/TLS origin.
- Multiple ports can be attached to the same electrical CEC domain. Address allocation and policy ownership belong to that domain.
- Source isolation is enforced before unauthorized CEC traffic reaches the switching device, even when the network or coordinator is unavailable.
- Every actuator checks current authority, boot, configuration, binding, domain and route generation before starting a frame. An already-started frame completes safely.
- Configuration commit, command receipt, command execution and observed equipment state are different events.
- Discovery, setup-answer saves and accepted suggestions do not wake or route equipment. **Try activity** is a separate explicit planner intent after required configuration activation; interrupted setup retains unaffected choices within the existing storage budget.
- No distributed “exactly once” claim is made for CEC toggles or relative key actions after an uncertain crash.
- Matter fabrics, HomeKit pairings, BLE remote bonds, administration credentials and consortium membership are separate security domains.
- Offline membership is retained. Removal is explicit. Neither coordinator loss nor removal causes a member to start independently controlling shared equipment.
- A five-tentacle hardware promise and a consortium-size promise require measured timing, memory, addressing and interoperability evidence.

## Initial scope

The initial qualification target is up to **five tentacles per supported octopus profile** and **eight octopuses per consortium**. Eight is a proposed test target, not an architectural limit or a requirement from the issues. A product build must advertise the smaller capacity it has actually qualified. Existing single-tentacle boards remain a supported special case.

Multi-zone control is part of the internal model. The initial Matter/HomeKit presentation remains one explicitly chosen primary zone; publishing additional zones requires separately qualified endpoints/accessories. ARC/eARC, TV applications, arbitrary HDMI matrix behavior, direct peer actuation, Wi-Fi radio mesh formation and automatic coordinator failover are separate future capabilities.

Physical setup gestures and their state eligibility follow [10](10-ble-and-matter-coexistence.md), including the explicit USB-assisted path for buttonless hardware. Local browser certificate trust remains a product/UX gate in [17](17-decisions-and-open-questions.md) and [20](20-user-experience-and-guided-setup.md); convenient labels, mDNS discovery and consortium enrollment do not make the existing HTTPS warning disappear.

No firmware was changed, built or exercised on hardware while preparing this package. Document consistency and fixture validation are recorded in `VALIDATION.md`; the hardware and integration tests described here are work to perform during implementation.
