# 17 — Decisions and open questions

**Status:** proposed architecture decision register, v0.1. Adopted recommendations below are concrete defaults for implementation. Unresolved gates require evidence before the affected capability is advertised. They are not hidden assumptions for coding agents to fill arbitrarily.

## 1. Adopted architecture decisions

| ID | Decision | Reason and consequence | Owner |
|---|---|---|---|
| A01 | One octopus contains 1–5 tentacles, each with two isolated CEC ports. | Preserves physical source isolation; maximum is ten engines, not five. | [01](01-octopus-architecture.md), [04](04-hardware-and-cec-runtime.md) |
| A02 | One shared CEC timer is the preferred runtime candidate. | Per-line GPTimers cannot reach ten on ESP32-S3; actual capacity still needs measurement. | [04](04-hardware-and-cec-runtime.md) |
| A03 | Electrical domains are explicit and independent of tentacles. | Ports can share a receiver/display bus; identity allocation must not collide. | [03](03-domain-model-and-addressing.md), [05](05-discovery-and-topology.md) |
| A04 | Persist qualified OSD names; resolve them to versioned runtime CEC addresses. ComponentId is the internal record reference. | Preserves Amity's automatic name-to-LA behavior with consortium/octopus/tentacle/side scope. LA changes update bindings, not configured names or activities; ambiguous substitution and stale commands are rejected. | [03](03-domain-model-and-addressing.md), [05](05-discovery-and-topology.md) |
| A05 | Source-edge semantic policy, no generic raw CEC bridge. | Prevents unwanted source activation before switch/display reaction. | [06](06-cec-roles-and-message-policy.md) |
| A06 | LAN consortium with designated coordinator in v1. | Gives one desired-state/resource authority without a premature embedded consensus subsystem. Partition loses new group control, not isolation. | [02](02-consortium-architecture.md), [13](13-faults-removal-and-recovery.md) |
| A07 | Member-initiated persistent mTLS/TCP, bounded JSON envelopes. | Fits existing IP environment, supports reliable framing and explicit trust without a broker. | [08](08-consortium-transport-and-routing.md) |
| A08 | Receiver-issued execution leases plus scoped generations. | Relative TTL alone cannot reject a very delayed packet safely across unsynchronized clocks. | [07](07-activity-planning-and-execution.md), [08](08-consortium-transport-and-routing.md) |
| A09 | Separate consortium setup secret and operational trust. | Matter PIN, HAP credentials, web credentials and remote bonds have different authorities. | [09](09-membership-security-and-enrollment.md) |
| A10 | Security2-compatible end-to-end enrollment over custom shared-host GATT. | Reuses a defined PAKE/channel design while avoiding a second NimBLE host; requires reviewed client implementation. | [09](09-membership-security-and-enrollment.md), [10](10-ble-and-matter-coexistence.md) |
| A11 | One RadioArbiter and one CHIP/Wi-Fi owner. | Matter induction, consortium onboarding and remote pairing share limited host/radio resources. | [10](10-ble-and-matter-coexistence.md) |
| A12 | Membership activation requires authenticated LAN proof and durable state. | BLE completion alone does not establish network reachability or recovery after interruption. | [09](09-membership-security-and-enrollment.md) |
| A13 | Topology configuration separates hardware, secrets and compiled member slices. | Portable backups must not clone security identities or unsafe pin maps. | [11](11-config-storage-and-migration.md) |
| A14 | Stable 16-slot external selector mapping; one primary external zone initially. | Preserves existing automation identities without assuming current endpoint resources support multiple theaters. | [11](11-config-storage-and-migration.md), [12](12-control-surfaces-and-admin.md) |
| A15 | Offline is retained; removed inline nodes retire passively. | Neither network loss nor removal grants independent control of shared equipment. | [13](13-faults-removal-and-recovery.md) |
| A16 | Planned coordinator transfer is quiesced, journaled and epoch-fenced. | No automatic timeout election; physical recovery handles uncertain authority. | [13](13-faults-removal-and-recovery.md) |
| A17 | Operation outcomes distinguish admission, wire application and observation. | CEC cannot supply generic exactly-once semantics or prove picture/audio from ACK. | [07](07-activity-planning-and-execution.md), [08](08-consortium-transport-and-routing.md) |
| A18 | Factory/origin identity survives intended reset; membership keys do not. | Supports stable device recognition and deliberate re-enrollment without stale authority. | [09](09-membership-security-and-enrollment.md), [11](11-config-storage-and-migration.md) |
| A19 | Separate semantic `config_revision` and cosmetic `presentation_revision`. | Label/order edits preserve live policies and holds; concurrent edits compare both revisions. | [03](03-domain-model-and-addressing.md), [11](11-config-storage-and-migration.md) |
| A20 | Persist last-requested sessions but require explicit resume after relevant reboot. | Avoids unexpected power/routing on restart and cannot replay uncertain actions. | [07](07-activity-planning-and-execution.md), [13](13-faults-removal-and-recovery.md) |
| A21 | Equipment title plus location context; screen-first guided setup with bounded resumable choices. | Optional naming, automatic supported defaults and reused connection answers minimize decisions. Board labels do not imply video routes; full scoped OSD selectors remain internal. Save and explicit Try are separate. | [20](20-user-experience-and-guided-setup.md), [11](11-config-storage-and-migration.md), [12](12-control-surfaces-and-admin.md) |
| A22 | Explicit physical first-board setup entry for qualified new hardware profiles. | Proposed short release requests Matter setup only for an eligible fresh singleton; group enrollment and reset have distinct longer holds. No shorter action starts during a longer hold, and fabric removal alone never reopens setup. Buttonless legacy preparation remains explicit. | [10](10-ble-and-matter-coexistence.md), [09](09-membership-security-and-enrollment.md) |

These decisions are recommendations for the requested design, not requirements already specified by the upstream issues. A change requires an explicit amended decision and matching interface/test changes.

## 2. Engineering gates

| Gate | Question to resolve | Owner / minimum evidence | Safe behavior while unresolved |
|---|---|---|---|
| G01 | Can one shared 50 μs interrupt service all ten lines under full radio/TLS/flash load? | Driver/hardware team: external waveforms, ISR duration/gap distribution, IRAM/link-map audit, stuck-line isolation. | Advertise only measured port capacity; preserve one-tentacle support. |
| G02 | Which ten GPIOs and electrical interface are safe on the chosen board? | Hardware team: schematic, boot/strap/USB/flash/PSRAM review, voltage/pull-up and unpowered behavior measurements. | No speculative five-tentacle pin profile; outputs remain released on invalid profile. |
| G03 | How many playback proxies do supported sinks accept on a shared domain? | CEC interoperability team: logical-address/version behavior for each qualified display/receiver. | Reject identity exhaustion; never reuse LA or masquerade as another type. |
| G04 | How is each sink-facing PA established, especially across dual AVR outputs? | Discovery team: staged source/output bootstrap, report provenance, confirmed fallback and recabling fixtures. | Required proxy remains unready; no guessed cross-domain PA announcement. |
| G05 | Which reviewed embedded Security2 client and exact transcript vectors will be used? | Security team: pinned protocol interoperability, proof/padding checks, malformed/timeout tests. | Consortium enrollment stays unavailable; no HomeKit proof substitution or plaintext fallback. |
| G06 | Can all commissioning/advertising entry points be admitted race-free through the pinned SDK? | BLE/Matter team: extra-service registration, global GAP/security callbacks, external Matter commissioning request tests. | Serialize workflows; if necessary create a narrowly scoped reviewed SDK hook, not another host. |
| G07 | Does consortium-only Wi-Fi persistence survive Matter fail-safe/boot paths? | Connectivity team: staged network commit/rollback, power cuts, zero-fabric reboot, cancellation of pending onboarding. | Candidate remains pending/unclaimed; no false activation or continual automatic Matter window. |
| G08 | Does exact-leaf mTLS work at offline cold boot with scoped time policy? | Security team: pinned backend tests for self-signed leaf trust, key proof, invalid certificate flags, time unavailable/rollback. | Reject unverifiable peers; never globally disable verification. |
| G09 | What node/port/configuration limits fit actual heap, sockets and flash journals? | Platform team: worst-case inventory, concurrent TLS/HTTPS/HAP/Matter/BLE, NVS power-cut/high-water tests. | Bound all counts and advertise a smaller supported cap. |
| G10 | What hold/step/scan deadlines work under real home network/CEC latency? | Control team: latency distribution and lost-message tests. Proposed 750 ms hold/250 ms renewal are targets. | Prefer bounded release and explicit timeout over unbounded repeats. |
| G11 | Which receiver output combinations, input selectors and source/display routes work? | AV qualification team: per-model capability matrix and actual HDMI observations. | Unknown capability is unavailable, not assumed from cable/socket count. |
| G12 | What production secure-boot, encrypted-storage and signed-OTA provisioning/recovery process is supported? | Platform/security team: approved key lifecycle, rollback/recovery drill and manufacturing instructions. | Development build is labeled; no implicit production claim or irreversible eFuse changes. |
| G13 | How do first-board setup, enrollment gestures, connector identification and indicators work on each supported profile? | Hardware/provisioning team: printed instructions, release/hold boundary and state-exclusion tests from document 10, recognizability tests, qualified indicator behavior, and explicit USB-assisted legacy upgrade. | No automatic setup reopening, no assumed universal button/LED, and no authenticated-web substitute for required physical presence. |
| G14 | Are all requested multizone semantics representable by the first external integration surface? | Integration team: primary-zone mapping, stable aliases/slots and household compatibility. | Publish only the explicit primary zone; extra external zones remain disabled. |
| G15 | What supported first-browser entry and trust path makes local setup usable? | Web/security/product team: exact URL/identity lifecycle and first-access procedure tested on supported phone/desktop browsers, including camera-unavailable code entry. | Document the actual local trust step; do not claim that QR/mDNS solves browser trust or silently replace peer enrollment trust with browser acceptance. |
| G16 | Can novices complete the proposed journeys with the specified minimum questions? | UI/integration team: UX-01 through UX-16 evidence, real-hardware completion, wrong-choice/help records, interruption recovery and narrow-screen accessibility. | Label the flow proposed until measured; reduce unsupported paths and remove avoidable repeated questions rather than inventing success. |

The eight-octopus target must be evaluated jointly with tentacle count and active integrations. Passing eight nodes with one quiet port does not establish eight nodes each running five busy tentacles and maximum web/TLS load.

## 3. Scope deliberately deferred

### ARC/eARC and built-in display applications

These are separate capabilities involving audio-system identities, System Audio/ARC behavior, HDMI sideband qualification and application/input control. Isolating CEC does not automatically solve them, and passing HDMI sidebands does not establish interoperability. No coding agent may enable raw frame forwarding to claim support. A follow-on specification must define hardware support, role model, routing/audio policies and test equipment.

### Automatic failover or fully decentralized peer actuation

A future design must define quorum/authority, durable terms, actuator fencing, stale leader recovery, membership changes under partition and the two-node case. Merely electing the lowest MAC or timing out a heartbeat is insufficient. A signed peer roster alone proves membership, not exclusive actuation authority.

### Additional external zones

Publishing several Matter media players/HomeKit television accessories requires stable endpoint/accessory identities, endpoint/resource budget, input selectors, household behavior and migration rules. The internal zone model is designed to support this later; it is not a claim that existing adapters already do.

### Transparent hardware and integration identity migration

Cloning Matter fabrics, HomeKit pairing databases or BLE bonds is not a replacement shortcut. A future supported migration must address protocol identity, key protection, storage consistency and ownership. This proposal preserves logical activities and components while making re-pairing requirements explicit.

### Wi-Fi mesh, ESP-NOW and network video

The consortium uses IP reachability. If a deployment requires a radio mesh or another backhaul, that is a separately qualified transport/admission problem. HDMI video transport and HDMI switching inside an octopus are outside this CEC control scope.

## 4. How to close a gate

Add the exact test/build/hardware versions, reproducible procedure, observed result, capacity/margin and decision to the validation record. Update the owning document, capability advertisement and user guidance. Failed evidence can close a gate by reducing scope: for example a three-tentacle supported profile is a valid outcome when five cannot meet timing/electrical limits.

Do not write “tested” for a simulated behavior that depends on real hardware. Do not infer source-theft prevention from a post-theft correction test. Keep explicit distinctions between design assertions, host tests, simulated network behavior, electrical captures and end-to-end equipment observations.
