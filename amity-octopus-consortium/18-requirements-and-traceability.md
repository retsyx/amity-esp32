# 18 — Requirements and traceability

**Status:** proposed v0.1. This table connects user/issue goals and derived correctness requirements to implementation ownership and concrete acceptance evidence. Requirement IDs should appear in implementation task descriptions and test names or metadata where practical.

## 1. Product requirements

| ID | Requirement and origin | Design owner | Required evidence |
|---|---|---|---|
| P01 | One octopus controls up to five HDMI pass-throughs. User request. | [01](01-octopus-architecture.md), [04](04-hardware-and-cec-runtime.md) | Five physical tentacles/ten concurrent CEC lines qualified on an explicit hardware profile, or clearly reduced advertised capacity. |
| P02 | Multiple displays, including a secondary receiver output with no useful CEC. [Issue 1](https://github.com/retsyx/amity/issues/1). | [02](02-consortium-architecture.md), [05](05-discovery-and-topology.md), [06](06-cec-roles-and-message-policy.md) | TV/projector can be selected independently where actual cable/resource capabilities allow; silent output does not block unrelated roles. |
| P03 | Sources connected directly to different display inputs can be selected. Issue 1. | [06](06-cec-roles-and-message-policy.md), [07](07-activity-planning-and-execution.md) | Independent per-input proxies and valid PAs select real wired inputs; no assumed TV-app/return-audio support. |
| P04 | Stop source theft before the switching device reacts. Issue 1. | [04](04-hardware-and-cec-runtime.md), [06](06-cec-roles-and-message-policy.md) | Protected-side capture shows absence of unauthorized activation/routing/standby, including boot/scan/partition; video observations distinguish non-CEC causes. |
| P05 | Coordinate physically distributed octopuses. [Issue 2](https://github.com/retsyx/amity/issues/2). | [02](02-consortium-architecture.md), [08](08-consortium-transport-and-routing.md) | Rack/room boards share one plan and state over authenticated LAN; remote can live away from actuator. |
| P06 | Addition/removal is easy for an end user. Original user request. | [09](09-membership-security-and-enrollment.md), [12](12-control-surfaces-and-admin.md), [13](13-faults-removal-and-recovery.md), [20](20-user-experience-and-guided-setup.md) | UX-03/12: guided Add + code + physical window + LAN proof; removal explains affected activities; no unnecessary per-node Matter induction. |
| P07 | Architecture documents support implementation by agents. Current user request. | [16](16-agent-work-breakdown.md), this package | Owned contracts, dependency graph, failure cases, work packages, fixtures and explicit gates. |
| P08 | Manage/configure the group through one understandable interface. Original user request. | [11](11-config-storage-and-migration.md), [12](12-control-surfaces-and-admin.md), [20](20-user-experience-and-guided-setup.md) | Title/location equipment cards, focused ambiguity resolution, screen-first activities, resource conflict explanation and one management session. |
| P09 | Secure communication and group management. Original user request. | [08](08-consortium-transport-and-routing.md)–[10](10-ble-and-matter-coexistence.md), [13](13-faults-removal-and-recovery.md), [14](14-ota-and-compatibility.md) | Authenticated onboarding/pins/roles, replay rejection, revocation, offline-time policy and production update/storage gates. |
| P10 | Minimize configuration throughout naming, equipment identification and activity selection. User's dedicated UX-document request. | [20](20-user-experience-and-guided-setup.md), 11/12/15 | UX-01 through UX-16: stable defaults, only unresolved questions, resumable choices, no hidden actuation, observed novice completion and question counts. |

## 2. Derived implementation requirements

| ID | Requirement | Design owner / work package | Acceptance case |
|---|---|---|---|
| D01 | Persisted QualifiedDeviceName includes consortium, octopus, tentacle, side and OSD name; resolved runtime endpoint uses the same prefix plus current LA. | [03](03-domain-model-and-addressing.md), W01 | Type substitution fails; duplicate names/addresses across distinct attachments stay distinct. |
| D02 | Component records persist qualified OSD names; ComponentId provides internal references and observations/bindings remain versioned. | 03, W01/W07/W08 | Name-to-LA resolution, receiver aliases, replacement and recabling preserve explicit configured selection. |
| D03 | Domain ownership is separate from connector ownership. | [05](05-discovery-and-topology.md), [06](06-cec-roles-and-message-policy.md), W06 | Several ports on one wire cannot claim conflicting TV/proxy identities. |
| D04 | Ten-line runtime does not allocate ten GPTimers. | [04](04-hardware-and-cec-runtime.md), W04 | Shared timer verified under simultaneous wire/radio/OTA load. |
| D05 | Passive observation does not ACK unowned addresses. | 04/06, W05 | Directed foreign frame captured with no observer ACK. |
| D06 | Scans are per domain, bounded, cancelable and nonblocking for release. | 05, W07 | Noisy unrelated domains cannot extend scan indefinitely; key release wins. |
| D07 | Physical-address evidence is scoped and ambiguity is explicit. | 05, W08 | Child-only/late/conflicting reports and dual-output paths do not cause guessed announcements. |
| D08 | No configuration/discovery side effect independently restores or powers a group route. | [07](07-activity-planning-and-execution.md), [11](11-config-storage-and-migration.md), [20](20-user-experience-and-guided-setup.md), W09/W12/W19 | UX-09: label edits, discovery, answer saves and suggestion acceptance do not wake/route/standby; Try is a separate intent after activation. |
| D09 | Shared receiver resources arbitrate across sessions. | 07, W11 | Two compatible sessions share; conflicting selector modes reject before effects. |
| D10 | Frame admission is rechecked at wire start. | [06](06-cec-roles-and-message-policy.md)–[08](08-consortium-transport-and-routing.md), W09/W17 | Queued old-generation Active Source never emits after policy switch. |
| D11 | Already-started frames have a bounded transition barrier. | 04/07, W04/W09 | Policy handover waits for complete/abort result and never cuts a frame arbitrarily. |
| D12 | TTL is supplemented by receiver-local ingress and execution leases. | 08, W13 | Delayed intent cannot create fresh authority; delayed operation is rejected even if received TTL is nonzero. |
| D13 | Receipt, application and observation remain distinct. | 07/08, W11/W13 | Missing CEC/report response never produces fabricated observed success. |
| D14 | Uncertain relative actions are not blindly replayed. | 07/08, W13/W18 | Crash after key step and before result yields unknown, not duplicate action. |
| D15 | Held keys expire locally; exact tokens protect newer holds. | 07, W18 | Loss/overflow/disconnect/route changes release within validated bound; late release A leaves hold B intact. |
| D16 | One shared NimBLE host and explicit workflow arbitration. | [10](10-ble-and-matter-coexistence.md), W14 | No duplicate host initialization; Matter, group and remote windows contend predictably. |
| D17 | Uncommissioned group members can join Wi-Fi without acquiring a Matter fabric. | 10, W16 | Zero-fabric reboot preserves committed network/group and does not reopen unintended Matter window. |
| D18 | Security2 client matches the pinned protocol, not merely the SRP name. | [09](09-membership-security-and-enrollment.md), W15 | Exact mutual proof/encryption vectors pass; HAP proof substitution fails. |
| D19 | Join activation follows end-to-end authentication and durable LAN proof. | 09/10, W16 | Power-cut matrix at every pending/commit transition resumes or rolls back correctly. |
| D20 | Operational peers use enrolled exact leaf keys and explicit roles. | 09, W13 | Rogue mDNS/CA descendant/replaced pin/wrong role are rejected. |
| D21 | Offline cold boot never becomes global TLS verification bypass. | 09, W13 | Missing clock permits only explicitly designed pinned-peer handling; all other failures still reject. |
| D22 | Configuration, factory, protocol and firmware versions are separate. | [11](11-config-storage-and-migration.md), [14](14-ota-and-compatibility.md), W12/W22 | Old config migration does not repurpose v3 factory reserved bytes or admit incompatible protocol. |
| D23 | External activity slots are stable across UI edits. | 11/[12](12-control-surfaces-and-admin.md), W12/W20 | Reorder/rename/delete do not retarget old Matter/HomeKit selectors. |
| D24 | Only the primary zone is published through initial external adapters. | 12, W20 | Secondary web/remote session does not overwrite primary integration state. |
| D25 | Offline, removed, retired and reset are distinct states. | [13](13-faults-removal-and-recovery.md), W21 | Disconnected member stays enrolled; removed inline board stays passive; reset clears new secrets. |
| D26 | Replacement creates fresh operational keys and binding revisions. | 13, W21 | Old queued commands and revoked pins cannot control the replacement; BLE bonds not cloned. |
| D27 | Coordinator movement is fenced; no timeout split brain. | 13, W21 | Power loss in each transfer phase cannot yield two executing coordinators. |
| D28 | OTA respects protocol/schema compatibility and safe local behavior. | 14, W22 | Interrupted mixed-version rollout preserves isolation, cancels holds and supports defined rollback. |
| D29 | Capacity is bounded and measured under combined load. | [15](15-validation-and-release-gates.md), W24 | Peak node/port inventory, concurrent security/radio, sockets, heap and flash headroom recorded. |
| D30 | Recovery and diagnostics do not leak secrets. | 09/12/13, W19/W21/W23 | Export/support bundle excludes credentials; intended factory origin survives documented reset only. |
| D31 | Cosmetic revision and semantic policy revision are separate. | 03/11/12, W12/W19 | User label/order edits preserve holds and leases; mixed/semantic changes use explicit fencing and owner acknowledgment. |
| D32 | Reboot does not automatically restore physical activity. | 07/13, W11/W21 | Saved sessions show suspended/degraded; new explicit intent validates and resumes without relative replay. |
| D33 | Output-mode-induced PA changes invalidate the remaining old plan. | 05/07, W08/W11 | Unstable output transition holds activation closed, refreshes evidence/bindings and creates a new plan. |
| D34 | Ordinary unambiguous LA churn automatically refreshes the same qualified OSD name without a configuration edit or confirmation. User naming clarification. | 03/05/07/08/11, W01/W07/W13 | LA 4 to 8 preserves names, ComponentId, activity and config revision; old binding work/holds are fenced, all capability bindings synchronized before fresh work. |
| D35 | Ambiguous same-scope OSD matches never fall back to a remembered LA; an old LA's new occupant cannot inherit the former component. | 03/05/15, W07/W12 | Duplicate, missing/renamed OSD and old-LA reuse fixtures; explicit resolution when evidence cannot distinguish devices. |
| D36 | Normal equipment selection uses title/location context, not qualified pathnames or raw CEC fields. | [20](20-user-experience-and-guided-setup.md), 03/12, W01/W19 | UX-04/05/14: full selector travels internally; connector context is added only when needed; raw sides/roles/domains stay advanced. |
| D37 | Guided setup carries forward known information and offers only validated screen/source/audio choices. | 20, 05/07/12, W07/W08/W11/W19 | UX-06/07/08: unresolved cable questions are targeted; shared AVR resources and unsupported routes are explained without making users configure internal roles. |
| D38 | Stable defaults and explicit user labels remain separate from OSD selectors and network identity. | 20, 03/11/12, W12/W19 | UX-04: refresh/discovery preserve overrides; board rename updates context without hostname/TLS-origin change; aliases render under one component. |
| D39 | Guided setup resumes a bounded nonsecret server draft under the existing configuration authority and budget. | 20, 11/12, W12/W19 | UX-11/15: retain completed choices across refresh/restart; invalidate only affected decisions; enforce shared draft/candidate 32 KiB cap, revision checks and no raw secrets. |
| D40 | Equipment aliases, offline state and replacement preserve valid user choices without inventing identity. | 20, 03/05/13, W07/W19/W21 | UX-05/10/12: one card per ComponentId, proxies hidden, LA churn automatic, offline records retained and genuine repair scoped to affected selectors. |
| D41 | Physical setup entry is one debounced, state-authorized gesture under the shared radio arbiter. | [10](10-ble-and-matter-coexistence.md), 09/20, W03/W13/W14/W16 | UX-01/03/06: below-3-second, 3-to-below-10-second and reset-threshold behavior, continuing holds and buttonless USB path qualify; setup alone never grants operational trust. |
| D42 | Local browser trust remains an explicit product/UX gate independent of consortium peer trust. | 12/17/20, W19/W25 | UX-01/03/16 record the actual supported-browser trust journey; no claim that labels/mDNS/enrollment remove certificate warnings and no insecure bypass. |
| D43 | Minimal configuration and accessibility require observed user evidence. | 20/15, W19/W25 | UX-14/16: novice completion/assistance and required/optional/repeated question counts recorded; narrow-screen journeys remain distinguishable without exposing internal paths. |

## 3. User-experience acceptance ownership

[Document 20](20-user-experience-and-guided-setup.md) owns the requirements and pass conditions for **UX-01 through UX-16**. The mapping below assigns implementation participation without creating a second set of UX definitions. [Document 15](15-validation-and-release-gates.md) owns evidence collection and release gates. These cases are requirements to validate, not claims of completed usability testing.

| Acceptance ID | Journey or contract owned by document 20 | Main work packages |
|---|---|---|
| UX-01 | First installation | W03/W14/W16/W19 |
| UX-02 | Existing installation migration | W12/W19/W20 |
| UX-03 | Additional board join | W03/W15/W16/W19 |
| UX-04 | Labels and qualified names | W01/W12/W19 |
| UX-05 | Duplicate names and proxy/alias deduplication | W07/W08/W19 |
| UX-06 | Discovery and identification | W03/W07/W08/W19 |
| UX-07 | Screen-first activity creation | W11/W19 |
| UX-08 | Shared AVR and multizone conflicts | W11/W17/W19 |
| UX-09 | Explicit Try and no hidden actuation | W09/W10/W11/W12/W19 |
| UX-10 | LA churn and identity repair | W01/W07/W13/W19/W21 |
| UX-11 | Interrupted setup | W12/W19 |
| UX-12 | Offline, removal and replacement | W19/W21 |
| UX-13 | Remote and integrations | W18/W19/W20 |
| UX-14 | Accessibility and narrow screens | W19 |
| UX-15 | Capacity and unsupported cases | W03/W12/W19/W24 |
| UX-16 | Novice usability and question budget | W19/W25 |

## 4. Evidence classes

Use these labels in validation records:

| Label | What it establishes | What it does not establish |
|---|---|---|
| Design review | Interfaces, threat/ownership reasoning and stated assumptions agree. | Code correctness or equipment behavior. |
| Host unit/property test | Deterministic model, parser and state-machine behavior. | Real timing, RF coexistence or pin electrical behavior. |
| Fault-injected simulation | Protocol ordering, retries, partitions, crashes and invariant preservation within the model. | Actual TLS/radio/CEC implementation fidelity by itself. |
| Target integration | Real firmware stacks and storage paths execute on ESP32. | Full AV interoperability with arbitrary equipment. |
| Electrical capture | Actual frames/timing/ACKs or absence at measured boundaries. | Picture/audio success unless separately observed. |
| End-to-end AV qualification | Behavior for named hardware, wiring and firmware versions. | Universal compatibility beyond that tested matrix. |
| Observed user session | Completion, assistance, questions and accessibility behavior for stated users/tasks/browser/hardware. | Universal ease of use, hardware correctness or a resolved browser-trust gate without matching evidence. |

The package's own `VALIDATION.md` records only document/fixture checks. It must not be confused with the repository's firmware validation record or with completed evidence for the requirements above.
