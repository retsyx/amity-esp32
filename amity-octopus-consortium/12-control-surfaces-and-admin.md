# Control surfaces and administration

Status: proposed implementation contract v0.1 against baseline `d50af8026938797fc0820d0107f4c4e064c2d2bc`.

## Purpose and scope

Users operate a system of activities and zones through one local browser interface, familiar remotes, and the existing home-automation integrations. They do not need to manage octopus-to-octopus connections or install a custom phone application. This document owns ingress adapters, external selector mapping, integration behavior and administration APIs. [User experience and guided setup](20-user-experience-and-guided-setup.md) is the normative owner of normal journeys, vocabulary, naming presentation, question order and UX-01 through UX-16. The [planner](07-activity-planning-and-execution.md), [transport](08-consortium-transport-and-routing.md), and [membership lifecycle](09-membership-security-and-enrollment.md) remain the authorities for execution and trust.

The baseline already queues remote and integration input into a single `HubService`, serves a local HTTPS UI, and exposes activities through Matter and HomeKit. Those are reusable adapter boundaries. The new system must remove assumptions that there is one front/back pair, one global activity index, and one local list of discovered devices. [Application composition][b-main] [Web status][b-web-status]

## One control facade

Introduce `SystemControl` as the application-facing interface implemented by `GroupService`. Its operations are conceptually:

- Read a bounded system/zone snapshot and capability summary.
- Submit a zone activity selection, repair request, or zone standby intent.
- Submit a component-directed semantic action authorized by the selected session.
- Start, renew, release, or cancel a tokenized hold.
- Start or inspect bounded discovery, validation, configuration, and maintenance jobs.

These are typed contracts using the [domain model](03-domain-model-and-addressing.md), not a second wire protocol. Standalone operation calls the same contracts through local transport. No adapter calls another octopus's raw CEC transmitter or constructs a substitute activity controller.

Every mutation identifies its origin, request/command identity and applicable revision. The facade distinguishes rejected, admitted, applied, and observed outcomes. Admission means work was accepted into bounded execution; it does not mean a projector is warmed up or a receiver selected its input. Adapters must return promptly without waiting on CEC, peer connectivity, a scan, or a full activity transition. Matter's existing callback interface already requires this enqueue-only behavior. [Matter callback contract][b-matter-api]

Snapshots expose per-zone desired activity, active session, transition status, applied semantic configuration revision, presentation revision, resource conflicts, and observation freshness. Equipment state can be unknown. Do not translate successful network delivery into a fabricated power/audio reading. The baseline maps a selected activity directly to integration power; the new adapter must state which values represent logical activity state and which require measured device evidence. [Baseline state publication][b-main-state]

Configured activities refer to component records containing `qualified_names`, whose entries are persistent `QualifiedDeviceName` selectors: `{consortium_id, octopus_id, tentacle_id, side, osd_name}`. ComponentId is an internal record reference. A UI adapter must not save a current logical address as a device choice or reduce the choice to an unscoped string. Snapshots can show the current `FullyQualifiedCecAddress` and runtime binding revision separately as discovery/execution status.

After coordinator restart, display persisted `last_requested_sessions` as `suspended_after_restart`; do not publish them as active or issue wake, route or standby commands automatically. A fresh Select/Resume intent resolves observations and creates a new SessionId/PlanId. A member reboot degrades its affected sessions and requires a fresh explicit intent to reconcile them; unaffected sessions continue. Ordinary reconnect within the same boot may restore input eligibility only after the planner verifies unchanged state, without replaying relative events. Make these distinctions visible in the activity view and external state mapping. [Restart behavior](07-activity-planning-and-execution.md)

## Device selection and user labels

The equipment view model distinguishes these values, exposing only the detail required by document 20's normal picker:

| Value | User-facing behavior | Persistence and meaning |
|---|---|---|
| Display label | The normal Rename action edits the label shown in activities/equipment. | Presentation-only; it does not change the OSD matching name or invalidate controls. |
| Matching device name | Normal cards show equipment title and octopus location label on two lines; add a connector only when needed to distinguish choices. Raw side belongs in advanced connection details. | Carry the complete `QualifiedDeviceName` internally; a name/scope change is a semantic edit with affected-activity preview. |
| Current logical address | Show only in detailed diagnostics or resolution status. | Runtime discovery value; no address-pin setting and no confirmation merely because it changes. |
| Proxy advertised name | Hide proxies from normal source choices; explain their representation only in advanced details. | Separate role/representation setting; it cannot replace the native source's matching name. |

For example, a normal card has **Apple TV** as its title and **Projector** as its second line when that is the octopus's location label. Do not display a qualified path or parse these strings back into a selector. Use ComponentId as the configured card's selection reference and carry the scoped selector through the view model/API. Render one equipment card per ComponentId; verified native aliases appear under expandable **Connections**, rather than becoming duplicate source choices. Shared-receiver aliases do not create multiple audio/resource owners.

The title uses an explicit user label when present, otherwise the configured primary OSD name or the stable default defined by document 20. Generated defaults and explicit overrides remain distinct. Discovery and derived activity titles must not overwrite user labels or rewrite OSD selectors. A board-label edit updates card context through OctopusId only; it does not rename the hostname, change the TLS origin or require a new browser trust step.

The canonical OSD-name value preserves the device's reported name representation; it is not derived from the display label, an ecosystem alias, or UI-normalized text. The configuration API enforces this separation even if a client submits misleading field names. Additional `qualified_names` aliases identify explicitly verified native attachments of the same component. A proxy runtime binding has no native `device_name` selector and is never offered as a substitute native device solely because its advertised name matches.

If the unique device selected as “Apple TV” on a particular octopus/connector/side changes from LA 4 to LA 8, discovery updates that record's runtime resolution automatically. Its activities, external input slots and labels continue to work without edits, confirmation or a new Select/Resume solely for the LA change. Status may briefly show resolution in progress; it must not report that the user replaced or renamed the configured equipment. The runtime fences work using the old binding and admits subsequent input using the new one; it never replays queued keys or hidden routing steps as a consequence of resolution.

Identical OSD names on different attachments appear with sufficient location/connector context to choose the intended device, using document 20's progressive disclosure. Equal names do not cause a global merge. If two native devices with the same name are visible inside the selected scope and no documented distinguishing constraint applies, show an ambiguous-device state and the affected activities. Changing their UI labels or selecting their current LAs cannot resolve the persisted ambiguity; guide the user to distinguish their reported OSD names where supported, place them on separately scoped isolated attachments, or use a supported matching rule whose evidence remains valid across LA changes. Copies of one device's observation seen by multiple monitors of a shared domain should be correlated; proxy details remain separate from native source choices. Missing OSD names and legacy fallback labels require an explicit unresolved state, not an implicit LA-based selection.

## External integration policy for version 1

**Exactly one primary zone is published through the existing Matter player/speaker/battery and HomeKit television.** Web control and remote bindings support multiple zones and concurrent sessions subject to resource arbitration. Publishing additional external zones/endpoints/accessories is a later compatibility gate, not something implemented by repeating the current fixed endpoint IDs.

Select the primary zone explicitly during migration or first integration setup. Existing installations migrate their original theater as that zone. The integration gateway is one designated octopus, initially the coordinator; gateway identity and coordinator authority remain separate concepts. A coordinator move does not move existing Matter/HomeKit credentials. If the gateway stays online, it may forward through the new coordinator after an authorized handover. Replacing the gateway requires the documented integration re-pairing workflow. [Recovery](13-faults-removal-and-recovery.md)

The current implementation creates Matter player endpoint 1, speaker endpoint 2 and remote-battery endpoint 3, and one HomeKit television with 16 input services. Preserve their identities on an in-place upgrade. Keep the existing MAC-derived HomeKit accessory serial for an already paired installation; OctopusId replaces application identity, not the accessory's identity behind an existing pairing. [Matter endpoints][b-matter-endpoints] [HomeKit construction][b-hap-construction]

Changing the published primary zone while pairings exist is not a normal rename operation. Version 1 requires an explicit remove/re-publish and re-pairing workflow, with a preview of affected automations. This avoids silently redirecting an existing television accessory to another room.

### Stable external activity slots

Keep a persistent registry of 16 slots. Each slot is `never_used`, `assigned(ActivityId)`, or `tombstone(ActivityId)`. Matter slot numbers remain 0–15; HomeKit identifiers remain the corresponding slot plus one. UI ordering and activity names are separate data.

| Change | Required external behavior |
|---|---|
| Rename or reorder an activity | Keep its slot, identity and controller-specific alias. |
| Temporarily unavailable activity | Keep assignment; reject selection with an unavailable result. |
| Delete an activity | Tombstone its slot; remove/mark it unconfigured as supported, and reject stale selections. |
| Restore the same ActivityId | Reuse its own tombstone, subject to valid primary-zone binding. |
| Create a different activity | Allocate a never-used slot; do not reuse another ActivityId's tombstone. |
| All 16 slots have been consumed | Keep the new activity web/remote-only; offer an explicit integration reset/re-pair workflow if slot reclamation is needed. |

Migration captures old positional mappings and HomeKit aliases/hidden flags before any reorder. The baseline stores HomeKit alias records under `hap_input_<index>` with an activity-name guard; the new registry keys the preference to ActivityId and gateway identity. Adapter code translates a slot into ActivityId before submitting intent. Passing a Matter byte index or HomeKit integer directly into an activity vector is prohibited. [Existing mappings][b-web-inputs] [Existing alias handling][b-hap-state]

The single external battery endpoint reports one explicitly selected primary-zone remote. It is not a synthetic average of all remotes. If that remote is disconnected, preserve unknown-state semantics. Other remote batteries remain visible in the browser. Additional battery endpoints require the same compatibility review as additional zones.

## Browser experience

The coordinator hosts the main UI and brokers authorized administration of members. The browser needs one session and one origin; it must not open a web session against every octopus. A member's recovery page may show its identity, local health and verified coordinator link, but must not become an alternate configuration writer. If the coordinator is unavailable, show that state and the supported recovery path rather than quietly accepting divergent changes.

These five functional areas support the journeys and vocabulary owned by document 20; they do not require five setup stages or five top-level destinations. The normal navigation is Activities, Equipment and Settings. Octopuses are under Equipment; remotes, integrations and maintenance are under Settings, with contextual links from affected equipment or activities:

1. **Activities:** select a zone and activity, see transition progress, repair the current selection, or stop that zone. Show why a shared receiver/source prevents a requested combination.
2. **Equipment and connections:** show discovered equipment and suggested connections; ask only the unresolved physical questions needed for the requested activity. Keep the Rename and Choose device actions distinct. The full graph editor, raw sides, roles, domains and logical/physical addresses belong in advanced connection details, not normal selection.
3. **Octopuses:** identify each node by its location label and show actionable connection/readiness status. Connector numbers appear when useful; per-port roles, domains and raw side information remain advanced details. An Identify control uses a supported LED or other validated signal; it must not pretend hardware has an indicator that is absent.
4. **Remotes and integrations:** pair a remote at a chosen owning octopus, bind its activity buttons/zone, select the primary integration zone and battery source, and manage Matter/HomeKit relationships.
5. **Maintenance:** group firmware plan, compatibility, configuration export/import, diagnostic download, removal/replacement, and recovery details.

“Offline,” “not yet enrolled,” “retired,” “configuration pending,” “attachment uncertain,” “ambiguous matching name,” and “CEC address conflict” are separate states. A green Wi-Fi icon does not imply that the HDMI control path is ready. A scan displays partial results and completion/timeout; the UI must not erase configured name selectors while a scan is in progress. Runtime resolution may become unavailable or change under the resolver's fencing rules without rewriting those selectors.

Membership setup follows the [enrollment document](09-membership-security-and-enrollment.md). The first board can continue using an existing Matter app to obtain Wi-Fi. Adding a member uses the group UI plus the new member's printed consortium code and physical authorization. QR camera capture is optional: provide manual entry and local image decoding. No browser Bluetooth API is required. Setup codes must not be placed in URLs, browser history, analytics, diagnostic logs, or third-party QR services.

Guided setup resumes the bounded nonsecret server draft defined in [configuration](11-config-storage-and-migration.md). Refresh/reconnect retain completed choices and re-evaluate only affected dependencies. Activity suggestions remain proposals until activation; saving setup answers is not an activity command. **Try activity** submits an explicit normal planner intent only after the required configuration is active. The adapter must not hide discovery-driven routing behind a setup progress screen.

### HTTPS and browser sessions

The baseline serves a self-signed per-board P-256 certificate, uses a password-derived administration record, and issues four in-memory bearer sessions lasting eight hours. The UI stores the bearer token in `sessionStorage`. Preserve session invalidation on password change and reset; do not distribute browser tokens to consortium peers. [HTTPS identity][b-web-tls] [Session implementation][b-sessions]

A consortium does not automatically solve the local browser certificate warning. Version 1 documents the existing local trust step honestly. Enrollment trust for peer leaf keys is separate and must not inherit the browser's acceptance of an unrelated certificate. Planned hostname/gateway changes must explain the new browser endpoint and session expiry, not claim that moving an mDNS name transfers trust.

Retain same-origin assets, restrictive content security policy, strict JSON input, no secret-bearing cache responses, and authorization on every privileged route. Password/setup-code controls require explicit user interaction and must clear temporary secret displays when the workflow closes.

## Administration HTTP contract

New functionality uses `/api/v2`. Existing `/api/setup` and `/api/login` may remain as the local authentication bootstrap. This table defines HTTP operations; peer messages use the separate envelope in document 08.

| Operation | Method and path | Contract |
|---|---|---|
| System snapshot | `GET /api/v2/system` | Bounded inventory, coordinator/gateway roles, zones, capabilities and applied revisions. |
| Configuration | `GET /api/v2/config` | Active portable document with persisted scoped OSD selectors and configuration/presentation revision pair; no authoritative LA mappings or security material. |
| Setup draft | `GET /api/v2/setup-draft`, `POST /api/v2/setup-draft` | Read/resume or update the single bounded nonsecret draft; compare draft revision and applicable configuration/presentation pair; return retained choices and decisions requiring refresh. No activation or actuation. |
| Validate candidate | `POST /api/v2/config/validate` | Strict validation and affected resources; no durable activation or CEC traffic. |
| Activate candidate | `POST /api/v2/config` | Requires `expected_config_revision`, `expected_presentation_revision` and request ID; returns tracked operation, not premature “all applied.” |
| Select/repair activity | `POST /api/v2/zones/{zone_id}/activity` | ActivityId, expected configuration revision and explicit repair flag. |
| Stop zone | `POST /api/v2/zones/{zone_id}/standby` | Releases only that session's resource ownership; shared resources remain protected. |
| Run discovery | `POST /api/v2/scans` | Explicit scope, deduplicated job, bounded results and scan ID. |
| Inspect operation | `GET /api/v2/operations/{operation_id}` | Stage/outcome, affected owners, retryability and freshness. |
| Membership | `POST /api/v2/membership/...` | Lifecycle-specific authorized operations from document 09/13. |
| Maintenance | `POST /api/v2/maintenance/...` | Explicit target and operation plan, including firmware and replacement. |

Mutation bodies use canonical IDs, a unique request ID and applicable revision preconditions. Repeated request IDs return the prior admitted result while retained; an expired dedup record is not a license to re-execute an uncertain non-idempotent action. Return a consistent error object with machine-readable code, user-facing message, affected IDs and retryability. Authentication failure, forbidden action, stale revision, unavailable owner, exhausted capacity and invalid configuration are distinct errors.

Pure labels and UI ordering advance only `presentation_revision`. Editing `qualified_names`, including any matching OSD name or attachment scope, is semantic. The server classifies the complete edit; the browser cannot claim that a selector, route, slot or capability change is merely cosmetic. Semantic edits advance `config_revision` and follow the activation/rebase transaction in [configuration](11-config-storage-and-migration.md). Automatic resolution of an unchanged scoped name to a different LA advances runtime binding state only and requires no configuration POST. All edit operations compare both revisions; activity selection compares the applicable semantic revision and, when confirming a resource conflict, the planner's expected runtime state revision.

Use bounded polling initially, consistent with the current four-second background refresh. A visible operation may poll faster under a bounded schedule; avoid one long-held socket per tentacle or job. Server-sent events/WebSockets require a separate socket budget. [Current polling][b-ui]

Legacy endpoints may be retained for representable single-tentacle installations during migration. In a configuration they cannot represent, return an explicit unsupported/conflict response. Do not silently map `/api/status` to the first octopus, reinterpret a v1 configuration as another zone, or let `/api/config` overwrite schema 2 with lossy data. The new UI uses v2 exclusively after migration.

## Remote ownership and event handling

Version 1 retains one managed BLE remote per octopus, matching the baseline transport, while allowing several octopuses' remotes to target different zones. A future multi-remote-per-octopus transport is a separate resource/coexistence change. Each remote has an owning OctopusId, stable RemoteId, zone binding, and a versioned button/activity map. [Current remote API][b-remote-api]

Decode HID/Siri reports and gestures at the owner. Send normalized intents through `SystemControl`; do not send proprietary raw reports to every peer. Preserve local physical held-button state so duplicate full-state reports cannot rearm a suppressed hold after an activity change. Current remote policy already quarantines stale activity generations and cancels work after queue/link loss. [Remote policy][b-remote-policy] [Policy tests][b-remote-tests]

Cross-octopus events add sender BootId/sequence, ingress identity, zone/session, route generation and target binding as appropriate. Local `esp_timer` timestamps are not comparable across octopuses. Member-originated intents require the coordinator-issued ingress lease; resulting operations require the actuator-issued execution lease, with the `realtime_input` class for key presses and positive hold renewals. Each receiver expires its own lease on its own monotonic clock. Do not wrap an old physical input event in a fresh lease after local queuing or reconnection. Buttons that select an activity map to ActivityId; UI reorder cannot change their meaning.

Every held action carries the scoped hold token from the [execution contract](07-activity-planning-and-execution.md). Initial targets are a 750 ms local lease and renewal at most every 250 ms, never a lease above two seconds. Disconnect, route change, ownership move or lease expiry cancels the matching hold. Release/cancel must bypass an overloaded ordinary queue and cannot release another newer owner's token. The baseline emergency-release behavior is a minimum local guarantee to retain. [Emergency release][b-release]

The UI's “move remote” operation shows the old/new owners, cancels active input, and requires re-pairing on the new owner. It does not clone bonds or infer ownership from strongest signal. Competing remotes targeting one zone are arbitrated by the coordinator; loss of the coordinator does not authorize a satellite to start a new cross-zone route on its own.

## Implementation work and acceptance

| Work item | Completion evidence |
|---|---|
| Add typed control/state facade | Web, Matter, HomeKit and remote adapters submit through one admission path; no CEC/network blocking in adapter callbacks. |
| Add primary-zone and stable-slot adapters | Existing pairings keep endpoint/accessory identity; reorder/rename/delete cannot retarget old selectors. |
| Replace singleton web data model | Two concurrent zones and five tentacles appear correctly; conflicting shared resources produce useful, stable errors. |
| Implement guided-setup adapters | Document 20's two-line cards, defaults, alias expansion and resumable choices work through bounded APIs; UX-01 through UX-16 have linked evidence. |
| Preserve name-based equipment selection | Activities operate after ordinary LA churn without editing/confirmation; duplicate scoped names remain distinct; same-scope ambiguity and proxy aliases cannot silently retarget selection. |
| Add remote binding/forwarding | Duplicated, delayed and stale events cannot control a new route; lost releases expire locally; moving a remote requires re-pairing. |
| Implement lifecycle UI | Join, offline, removal, replacement and coordinator-unavailable states are distinguishable without technical guesswork. |
| Preserve boundary security | Session/CSRF/content-type/secret-log tests pass; peer pins and web session credentials remain separate. |

Browser fixture tests alone do not prove hardware control. Validate UI against fake APIs, then firmware APIs, then real Matter/HomeKit controllers and the multi-port HDMI fixture. Gateway replacement and additional external zones stay gated until their compatibility behavior has been demonstrated. See [validation](15-validation-and-release-gates.md).

[b-main]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/main/main.cpp#L37-L113
[b-web-status]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/web_admin.cpp#L28-L103
[b-matter-api]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_matter/include/amity_matter.h#L29-L77
[b-main-state]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/main/main.cpp#L267-L272
[b-matter-endpoints]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_matter/amity_matter.cpp#L323-L385
[b-hap-construction]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_homekit/homekit.cpp#L158-L196
[b-web-inputs]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/web_admin.cpp#L37-L40
[b-hap-state]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_homekit/homekit.cpp#L70-L99
[b-web-tls]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web/web.cpp#L21-L76
[b-sessions]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web/credentials.cpp#L18-L108
[b-ui]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/assets/app.js#L546-L554
[b-remote-api]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/ble_remote/include/ble_remote.h#L20-L67
[b-remote-policy]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/ble_remote/include/remote_input.hpp#L20-L78
[b-remote-tests]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/remote_input_test.cpp#L68-L155
[b-release]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub_service.cpp#L444-L451
