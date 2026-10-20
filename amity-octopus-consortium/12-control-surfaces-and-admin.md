# Control surfaces and administration

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

Status: proposed implementation contract v0.1 against baseline `d50af8026938797fc0820d0107f4c4e064c2d2bc`.

## Purpose and scope

Users operate a system of activities and zones through the baseline local browser interface, the sole original-controller remote, and the existing home-automation integrations. They do not need to manage octopus-to-octopus connections or install a custom phone application. This document owns ingress adapters, external selector mapping, integration behavior and administration APIs. [Baseline user interface](20-user-interface.md) is the normative owner of baseline UI behavior, vocabulary and device-list presentation and UX-01 through UX-16. The [planner](07-activity-planning-and-execution.md), [transport](08-consortium-transport-and-routing.md), and [membership lifecycle](09-membership-security-and-enrollment.md) remain the authorities for execution and trust.

The baseline already queues remote and integration input into a single `HubService`, serves a local HTTPS UI, and exposes activities through Matter and HomeKit. Those are reusable adapter boundaries. The new system must remove assumptions that there is one front/back pair, one global activity index, and one local list of discovered devices. [Application composition][b-main] [Web status][b-web-status]

## One control facade

Introduce `SystemControl` as the application-facing interface backed by the owner-thread HubService/LiveSystem and coordinator. Its operations are conceptually:

- Read a bounded system/zone snapshot and capability summary.
- Submit a zone activity selection, repair request, or zone standby intent.
- Submit a component-directed semantic action authorized by the selected session.
- Start, end, or cancel a tokenized held state; there is no per-key renewal.
- Start or inspect bounded discovery, validation, configuration, and maintenance jobs.

These are typed contracts using the [domain model](03-domain-model-and-addressing.md), not a second wire protocol. Standalone operation calls the same contracts through local transport. No adapter calls another octopus's raw CEC transmitter or constructs a substitute activity controller.

Every mutation identifies its origin, request/command identity and applicable revision. The facade distinguishes rejected, admitted, applied, and observed outcomes. Admission means work was accepted into bounded execution; it does not mean a projector is warmed up or a receiver selected its input. Adapters must return promptly without waiting on CEC, peer connectivity, a scan, or a full activity transition. Matter's existing callback interface already requires this enqueue-only behavior. [Matter callback contract][b-matter-api]

Snapshots expose per-zone desired activity, active session, transition status, applied semantic configuration revision, presentation revision, resource conflicts, and observation freshness. Equipment state can be unknown. Do not translate successful network delivery into a fabricated power/audio reading. The baseline maps a selected activity directly to integration power; the new adapter must state which values represent logical activity state and which require measured device evidence. [Baseline state publication][b-main-state]

Configured activities refer to component records containing `qualified_names`, whose entries are persistent `QualifiedDeviceName` selectors: `{consortium_id, octopus_id, tentacle_id, side, osd_name}`. ComponentId is an internal record reference. A UI adapter must not save a current logical address as a device choice or reduce the choice to an unscoped string. Snapshots can show the current `FullyQualifiedCecAddress` and runtime binding revision separately as discovery/execution status.

After coordinator restart, display persisted `last_requested_sessions` as `suspended_after_restart`; do not publish them as active or issue wake, route or standby commands automatically. A fresh Select/Resume intent resolves observations and creates a new SessionId/PlanId. A member reboot degrades its affected sessions and requires a fresh explicit intent to reconcile them; unaffected sessions continue. Ordinary reconnect within the same boot may restore input eligibility only after the planner verifies unchanged state, without replaying relative events. Make these distinctions visible in the activity view and external state mapping. [Restart behavior](07-activity-planning-and-execution.md)

## Device selection and user labels

Preserve the baseline device listings, activity fields and datalist/selection behavior. Add only the owning Amity hostname and HDMI-adapter context to distinguish equipment. Do not introduce an Equipment page, Connections wizard or new Rename/Choose-device workflow. The backend carries full QualifiedDeviceName/ComponentId references behind the unchanged form; display strings are never parsed back into a current LA or reduced to global bare-name matching.

Component/activity labels remain presentation-only. An Amity-device name is its actual canonical hostname, not a separate presentation alias. Scoped OSD selectors are semantic; unique LA churn resolves automatically without an activity edit, while same-scope ambiguity remains unavailable. Proxy representations cannot masquerade as native source choices. Duplicate equipment names on different attachments retain their separate IDs and complete scope. Existing AVR/audio/switch fields and their validated semantics remain supported; an adapter must preserve metadata absent from the baseline form rather than erase it.

## External integration policy for version 1

**Exactly one primary zone is published through the existing Matter player/speaker/battery and HomeKit television.** The original controller's web control and sole remote support multiple zones and concurrent sessions subject to resource arbitration. Publishing additional external zones/endpoints/accessories is a later compatibility gate, not something implemented by repeating the current fixed endpoint IDs.

Retain the saved/migrated primary zone behind the baseline interface; do not add a new primary-zone chooser. The original first-setup Amity is the immutable integration/UI gateway and controller. Added devices cannot host another home integration, UI or input bridge. Changing a screen does not move authority or pairing ownership. Live controller-ID edits and promotion/handover are unsupported. Failed-main recovery uses freshly Matter-enrolled replacement hardware and existing Advanced upload/restore, with explicit old-main isolation, new authority, re-pairing and headless-member re-enrollment; it does not clone the original hardware identity or pairings.

The current implementation creates Matter player endpoint 1, speaker endpoint 2 and remote-battery endpoint 3, and one HomeKit television with 16 input services. Preserve their identities on an in-place upgrade. Keep the existing MAC-derived HomeKit accessory serial for an already paired installation; OctopusId replaces application identity, not the accessory's identity behind an existing pairing. [Matter endpoints][b-matter-endpoints] [HomeKit construction][b-hap-construction]

The internal published primary projection must not silently redirect paired automation. Preserve its migrated setting and original UI; the amendment does not add a primary-zone chooser or integration redesign.

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

The single external battery endpoint describes the sole remote paired to the original controller, never a member remote or synthetic average. Preserve unknown/unavailable state when disconnected. Zone selection does not change the physical pairing owner.

## Browser experience

Only the original controller hosts the browser, login, setup, admin and recovery UI and brokers member administration. Added devices host no UI of any kind, including a read-only status/recovery page. Their status/configuration/updates travel through authenticated peer channels to the original controller; explicitly authorized physical/USB bootstrap remains separate. If the original controller is unavailable, user control/UI is unavailable until it recovers; a member cannot accept divergent changes.

The UI preserves baseline tab contents and workflows, with the approved navigation order **Activities, Remote, Matter, HomeKit, Amity devices, Advanced**. Advanced is last, immediately after Amity devices; this is the explicit 21 October 2026 ordering exception. Canonical Amity-hostname/adapter context and one additional Amity management tab are added; the explicit per-adapter settings amendment moves only the former global Advanced default front address into that tab's per-device Settings dialog. Internal scoped APIs do not require exposing new form fields, remote mapping or primary-zone controls.

“Offline,” “not yet enrolled,” “retired,” “configuration pending,” “attachment uncertain,” “ambiguous matching name,” and “CEC address conflict” are separate states. A green Wi-Fi icon does not imply that the HDMI control path is ready. A scan displays partial results and completion/timeout; the UI must not erase configured name selectors while a scan is in progress. Runtime resolution may become unavailable or change under the resolver's fencing rules without rewriting those selectors.

Membership setup follows the [enrollment document](09-membership-security-and-enrollment.md). The first board can continue using an existing Matter app to obtain Wi-Fi. Adding a member uses the group UI plus the new member's printed consortium code and physical authorization. QR camera capture is optional: provide manual entry and local image decoding. No browser Bluetooth API is required. Setup codes must not be placed in URLs, browser history, analytics, diagnostic logs, or third-party QR services.

The original activity editor retains its ordinary unsaved edits, Save/Discard and explicit Start controls. Save, Scan and device-list refresh never authorize playback. An explicit Start is admitted only against current committed configuration and normal planner/resource checks. No separate server setup draft, guided route workflow or Try wizard exists.

### HTTPS and browser sessions

The baseline serves a self-signed per-board P-256 certificate, uses a password-derived administration record, and issues four in-memory bearer sessions lasting eight hours. The UI stores the bearer token in `sessionStorage`. Preserve session invalidation on password change and reset; do not distribute browser tokens to consortium peers. [HTTPS identity][b-web-tls] [Session implementation][b-sessions]

A consortium does not automatically solve the local browser certificate warning. Version 1 documents the existing local trust step honestly. Enrollment trust for peer leaf keys is separate and must not inherit the browser's acceptance of an unrelated certificate. An explicitly changed main hostname must explain the browser endpoint and session expiry; changing an mDNS name never transfers trust.

Retain same-origin assets, restrictive content security policy, strict JSON input, no secret-bearing cache responses, and authorization on every privileged route. Password/setup-code controls require explicit user interaction and must clear temporary secret displays when the workflow closes.

## Administration HTTP contract

New functionality uses `/api/v2`. Existing `/api/setup` and `/api/login` may remain as the local authentication bootstrap. This table defines HTTP operations; peer messages use the separate envelope in document 08.

| Operation | Method and path | Contract |
|---|---|---|
| System snapshot | `GET /api/v2/system` | Bounded inventory, original-controller/actuator roles, zones, capabilities and applied revisions. |
| Configuration | `GET /api/v2/config` | Active portable document with persisted scoped OSD selectors and configuration/presentation revision pair; no authoritative LA mappings or security material. |
| Save activities | Existing baseline Save through strict activity-editor adapter | Exact configuration/presentation/Boot, stable logical IDs and scoped device-choice tokens; no playback. |
| Start activity | Existing baseline Start through the activity-editor adapter | Stable ActivityId, current state/context checks and stop barrier where required. |
| Scan HDMI | Baseline discovery through the original controller | Bounded local/actuator scans; no wake/routing. |
| Inspect operation | Tracked activity/editor operation status | Admitted/applied/observed/failed remain distinct. |
| Device inventory and actuator management | Amity devices tab through authenticated controller APIs | Main is always first with the inline heading `<hostname> (this device)` and right-aligned Settings in that title row, including without members; added-device Settings and Remove/Replace are also right-aligned in the title row, with Update firmware permitted below; no member Remove/Replace/Update actions on main. Add requires a valid Name that sets the candidate hostname. Remove factory-resets the selected responding added device; an unresponsive device is dropped locally with physical-reset instructions. There is no independent Location alias; actuator status/remove/replace/update stay explicit and members have no UI. |
| Device settings | `GET/POST /api/v2/devices/<OctopusId>/settings` | Exact named owner, boot/settings/presence generations; read-only inspection or explicit selected-device save/restart, as specified below. |
| Restore configuration | Existing Advanced upload/restore | Current CAS/Boot, explicit old-main isolation for fresh-main recovery; no key/trust cloning or activity start. |

Add uses a closed `POST /api/v2/membership/add` body `{request_id, setup_code, name}`. Name is required and normalized to lowercase after trimming surrounding whitespace; internal spaces and punctuation are rejected, never converted into a slug. Validate the 1–32-character DNS-label rules and duplicate assigned names before radio work. `GET /api/v2/system` exposes the main `hostname`, authenticated peer `hostname` fields, and durable `named_devices: [{octopus_id, hostname}]` metadata for known offline/removed devices. Use actual names for rows and equipment context; an unknown name is **Name unavailable**, never an old cosmetic label. Main hostname changes remain in Advanced; this amendment adds no member rename action.

Remove uses the closed `POST /api/v2/membership/remove` body `{request_id, octopus_id, confirm:true, expected_membership_revision, expected_target_boot_id}`. Capture the decimal membership revision and target BootId when the confirmation dialog opens, from `membership.membership_revision` and `membership.peers[].boot_id`; an offline target may have `null` BootId. Do not silently refresh these reviewed tokens. Atomically reject a changed membership revision, or a different BootId on a currently responding target, before revoking anything. A target that has become unresponsive may still be dropped locally; no reset command survives for a later reconnection. The main cannot remove itself. A responding peer without the negotiated reset capability must be updated before removal.

Poll the retained operation result: `stage` is `admitted` or `complete`, while `reset` independently reports `scheduled`, `offline` or `unconfirmed`. Completion means the main durably dropped the device; `scheduled` requires the exact authenticated durable reset-marker acknowledgment and does not prove erasure finished. Keep the result visible. For offline or unconfirmed reset, tell the user to hold the device's physical button for at least ten seconds before adding it again. A lost HTTP result requires explicit list refresh and review, never automatic replay. [Removal lifecycle](13-faults-removal-and-recovery.md#intentional-removal).

Mutation bodies use canonical IDs, a unique request ID and applicable revision preconditions. Repeated request IDs return the prior admitted result while retained; an expired dedup record is not a license to re-execute an uncertain non-idempotent action. Return a consistent error object with machine-readable code, user-facing message, affected IDs and retryability. Authentication failure, forbidden action, stale revision, unavailable owner, exhausted capacity and invalid configuration are distinct errors.

Pure component/activity labels and UI ordering advance only `presentation_revision`. Editing `qualified_names`, including any matching OSD name or attachment scope, is semantic. The server classifies the complete edit; the browser cannot claim that a selector, route, slot or capability change is merely cosmetic. Semantic edits advance `config_revision` and follow the activation/rebase transaction in [configuration](11-config-storage-and-migration.md). Automatic resolution of an unchanged scoped name to a different LA advances runtime binding state only and requires no configuration POST. All edit operations compare both revisions; activity selection compares the applicable semantic revision and, when confirming a resource conflict, the planner's expected runtime state revision.

## Per-device settings contract

`GET /api/v2/devices/<OctopusId>/settings` is non-actuating. Return the closed projection `{status, settings, error}`, where status is `ready`, `reading`, `offline`, `unsupported` or `error`, and settings is null when no usable snapshot exists. A snapshot is `{boot_id, settings_revision, adapters}`. Each adapter is `{tentacle_id, name, configured, presence, presence_generation, default_front_physical_address, observed_front_physical_address, effective_front_physical_address}`. UI snapshots retain the hardware profile's supported stable slots: adapter 1 for fixed one-adapter hardware, or all five unique slots 0–4 named **HDMI adapter 1** through **HDMI adapter 5** for a five-adapter profile, including unconfigured holes. Do not invent slots beyond advertised capacity. Revisions/generations are canonical decimal uint64 strings; presence is `present|absent|unknown`; runtime addresses are dotted addresses or null. Saved defaults remain visible independently of runtime evidence. Remote reads use authenticated peer metadata, never a member web listener, and do not trigger CEC scans.

`POST` to the same path accepts only `{request_id, expected_boot_id, expected_settings_revision, confirm_restart:true, changes}`. Changes contain 1–5 unique `{tentacle_id, expected_presence_generation, default_front_physical_address}` records, with nonzero expected presence generations. Blank address resets that connector's default to 1.0.0.0; otherwise require a hierarchical dotted CEC address (no 0xFFFF or nonzero nibble after a zero). Reject missing/unknown fields, overflow, duplicate slots and invalid owner identity before effects. The server captures the original local admission deadline before parsing the body; queuing cannot create fresh authority.

Return tracked admission and inspect `/api/v2/operations/<request_id>`. Admission is not persistence. Only exact durable applied success is success; busy, stale Boot/settings/presence, expired/revoked authority, storage error and unknown completion remain distinct. No browser automatic resubmission after uncertainty. At the owner, reserve admission, quiesce and drain before the callback; resample presence and reject removal/reinsertion ABA before writing. Bounded persistence must return its actual result once started. Success keeps execution closed and authorizes only the selected device's restart after the response transmission attempt, including a failed attempt; no restart on mere admission. The user already authorized that restart with `confirm_restart:true`.

Offline/unknown/absent/unsupported states are read-only, with retained defaults but no invented physical presence. [Document 20](20-user-interface.md#per-device-hdmi-adapter-settings-amendment--20-october-2026) defines labels and the acceptance matrix. The design [settings schema](schemas/adapter-settings.schema.json) projects these fields; live authority, generation equality and electrical truth remain semantic checks.

Use bounded polling initially, consistent with the current four-second background refresh. A visible operation may poll faster under a bounded schedule; avoid one long-held socket per tentacle or job. Server-sent events/WebSockets require a separate socket budget. [Current polling][b-ui]

Legacy endpoints may be retained for representable single-tentacle installations during migration. In a configuration they cannot represent, return an explicit unsupported/conflict response. Do not silently map `/api/status` to the first octopus, reinterpret a v1 configuration as another zone, or let `/api/config` overwrite schema 2 with lossy data. The restored baseline UI may use compatibility endpoints only through a strict schema2 adapter that preserves stable references, revision preconditions and unrepresented metadata. It must not activate an independent legacy controller or silently overwrite a richer configuration.

## Remote ownership and event handling

The original first-setup controller owns the sole managed BLE remote and its local bond. Its unchanged baseline button/activity behavior can select activities/screens throughout the system. Configuration has no remote-owner/button-map records, and APIs provide no delegation. No Move remote or Move coordinator workflow exists. See [original-controller policy](21-original-controller-policy.md).

Decode HID/Siri and physical held-button state only on the original controller. Normalize into SystemControl intents with original input context, ordered sequence, exact hold token and receiver deadline. Duplicate reports cannot rearm a suppressed hold. Target-zone/route change cancels affected presses without moving a gesture; activity identities and external selectors survive UI reordering.

Controller-to-actuator operations retain current BootIds, policy/binding/route fences and receiver-issued execution leases. Independent clocks are never compared. There are no member user-input, ingress-lease or remote-management wire/API paths; old queued work cannot adopt a new controller role or deadline.

Each held action uses its exact scoped token. Preserve the original remote algorithm: normalize physical down/up transitions on the original controller, transport those state changes once, and repeat CEC presses at the owning output using the original completion-paced loop. Repeated reports of the same down state do not send periodic presses or renewals. A tap is a finite press whose completion ends its pressed state. The executor derives CEC release from the receiving device's stream end only when all keys there are no longer pressed; remote-up updates state and is not a direct release frame. Disconnect, lost-input cancellation, route change or controller/link/plan-authority loss cancels the matching hold; release bypasses ordinary queue overload and never targets a newer gesture. Initial input/delivery deadlines apply before the first press, not as a duration limit on a continuing intentional hold. The sole remote may change target screen while remaining paired to the same controller; no bond or home-integration identity is cloned. [Original algorithm contract](07-activity-planning-and-execution.md#7-keys-and-remote-input).

## Implementation work and acceptance

| Work item | Completion evidence |
|---|---|
| Add typed control/state facade | Web, Matter, HomeKit and remote adapters submit through one admission path; no CEC/network blocking in adapter callbacks. |
| Add primary-zone and stable-slot adapters | Existing pairings keep endpoint/accessory identity; reorder/rename/delete cannot retarget old selectors. |
| Replace singleton web data model | Two concurrent zones and five tentacles appear correctly; conflicting shared resources produce useful, stable errors. |
| Preserve baseline UI adapters | Document 20's amended baseline activity fields/tabs and sole added management tab match the reference; no guided wizard or new owner/primary-zone choices appear. |
| Preserve name-based equipment selection | Activities operate after ordinary LA churn without editing/confirmation; duplicate scoped names remain distinct; same-scope ambiguity and proxy aliases cannot silently retarget selection. |
| Enforce sole original-controller remote | SC-01–SC-08: member/second remote/input/owner-change requests reject; one remote selects multiple screens; stale gestures never retarget and exact releases expire locally. |
| Implement lifecycle UI | Join, offline, removal, replacement and coordinator-unavailable states are distinguishable without technical guesswork. |
| Preserve boundary security | Session/CSRF/content-type/secret-log tests pass; peer pins and web session credentials remain separate. |

Browser fixture tests alone do not prove hardware control. Validate UI against fake APIs, then firmware APIs, then real Matter/HomeKit controllers and the multi-port HDMI fixture. Manual replacement-main restore requires SC-08 software/physical evidence; additional external zones remain separately gated. See [validation](15-validation-and-release-gates.md).

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
