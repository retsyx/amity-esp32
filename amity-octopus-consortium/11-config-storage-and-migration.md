# Configuration, storage, and migration

Status: proposed implementation contract v0.1. Baseline: `d50af8026938797fc0820d0107f4c4e064c2d2bc`. Requirements below describe the proposed implementation; they are not claims about released firmware.

## Purpose and ownership

This document owns configuration validation, persistent records, activation, migration, export, and reset coverage. The [domain model](03-domain-model-and-addressing.md) owns identifiers and topology semantics; [activity planning](07-activity-planning-and-execution.md) owns equipment transitions; [membership security](09-membership-security-and-enrollment.md) owns enrollment authority and credentials. [User experience and guided setup](20-user-experience-and-guided-setup.md) is the normative owner of normal user journeys, vocabulary, naming presentation and question order. Storage code must not invent a second membership or coordinator protocol.

The existing application persists a single version 1 configuration containing names of a display, source, audio device, and optional switch. The parser accepts at most 16 activities and rejects additional top-level fields. Its storage wrapper uses the `amity_cfg/amity` NVS namespace, permits 32,768-byte writes, and commits one blob at a time. These are useful starting constraints, not a distributed configuration system. [Baseline parser][b-parser] [Baseline store][b-store]

## Independent version axes

| Version or revision | Meaning | Required behavior |
|---|---|---|
| Configuration schema version | Structure of portable configuration; proposed value `2` | Unsupported major schema is rejected before persistence or actuation. |
| Configuration revision (`config_revision`) | Coordinator-assigned uint64 generation of executable meaning | Encode as canonical decimal text in JSON; changes require semantic activation. |
| Presentation revision (`presentation_revision`) | Coordinator-assigned uint64 generation of user labels and UI ordering | Encode as canonical decimal text; changes do not invalidate local policy, leases or holds. |
| Binding/domain revision | Current resolution of a configured name selector, or interpretation of an electrical domain | A mismatch invalidates work referring to the old runtime attachment; ordinary name-to-LA resolution does not edit configuration. |
| Consortium protocol version | Peer message compatibility | Negotiate under the [transport contract](08-consortium-transport-and-routing.md). |
| Factory record version | Manufacturing/bootstrap representation | Parse independently; do not infer it from firmware version. |
| Firmware version | Executable release | Apply [OTA compatibility](14-ota-and-compatibility.md), including readable storage versions. |

OctopusId, ConsortiumId, ComponentId and ActivityId remain stable across display-label edits and list reordering. ComponentId is internal referential bookkeeping; it does not replace the configured device's persistent `QualifiedDeviceName` selector. A BootId is recreated at each boot. An observed logical or physical CEC address is never substituted for that persistent selector.

Routes, configured name selectors and binding policies, capabilities, activity creation/deletion, integration slot assignments and quirks change `config_revision`. Pure user-label and UI-order edits change only `presentation_revision`. An ordinary change in the LA currently resolved from an unchanged name selector changes runtime binding revisions, not either configuration revision. A mixed configuration edit increments every changed axis and follows semantic activation. All configuration edits compare the expected pair so two editors cannot overwrite each other's changes merely because they touched different kinds of fields. The complete stored/exported payload digest covers both axes; the separate executable-policy hash excludes presentation data and revision labels, while including every configured selector, binding policy, role, capability and policy value that can affect execution. Runtime resolved-address and binding-generation checks remain separate from that hash.

## Configuration contract

Schema 2 describes zones, activities, configured component records with persistent name selectors, binding policies, domain/port roles, resource relationships, and explicit control capabilities using the canonical records in the domain model. Static configuration is distinct from resolved runtime bindings, active sessions, observations, pending commands, and security membership.

### Persist names; resolve addresses at runtime

Every configured CEC component record MUST persist `qualified_names: QualifiedDeviceName[]`, with at least one `{consortium_id, octopus_id, tentacle_id, side, osd_name}` selector. The primary selector comes first; additional native attachment aliases require explicit verification. A non-CEC or unresolved placeholder may have an empty list, but it cannot acquire a native CEC control binding until its selector is configured. Activities reference these name-bearing records through their internal ComponentIds; neither activities nor their records persist an LA as the device-selection key. An internal ID without its persisted name selector is insufficient to restore a configured CEC device.

`FullyQualifiedCecAddress` replaces `osd_name` with `logical_address` in the same scope and is a runtime observation/execution address only. Discovery resolves a configured name selector to the current matching native observation and constructs the revision-fenced runtime binding. A native runtime binding includes `device_name: QualifiedDeviceName` matching one of its component's `qualified_names` and the binding endpoint's entire port scope. A proxy binding has no `device_name`; it derives its representation from an explicitly configured native component and proxy role. A diagnostic snapshot may report the resolved address, but configuration import must not treat it as an address pin or alternative selector.

Matching uses the canonical OSD-name representation from [the domain model](03-domain-model-and-addressing.md), preserving the reported name bytes without case folding, trimming, fuzzy matching or substitution of the user's display label. The editable label, a HomeKit alias and a proxy's advertised name are distinct fields with different meanings. Renaming a UI label must never rewrite `osd_name`. Missing, unrepresentable or ambiguous OSD identity leaves the selector unresolved; there is no fallback to a remembered LA, a default LA-derived name or the first global name match.

Presentation metadata distinguishes a stable generated default from an explicit user label override. Retain accepted defaults and overrides across refresh, discovery and restart; derived card/activity titles do not overwrite a user's label or mutate an OSD selector. A board's location label is presentation metadata, not its hostname or HTTPS origin. Renaming that label must not rewrite network names, issue a replacement TLS certificate or expire the browser's origin merely to match the visible text. Document 20 defines the display rules.

Identical OSD names on different scoped attachments are valid and remain separate selectors. Two native candidates with the same name inside the selector's scope are ambiguous until their reported names, attachment scopes or an explicitly documented and tested matching constraint make the selection unambiguous. Such a constraint belongs to persisted matching policy and must remain valid when LAs change; it cannot be a remembered LA. Correlating copies of one native observation received by several monitors on the same domain does not create another appliance. Conversely, multiple native devices visible through the same shared domain cannot be disambiguated merely by selecting which monitor saw them. Locally generated proxies and configured representation aliases are typed as such; they are not native discovery candidates that can replace their source's native selector. A proxy/native name collision or proxy alias equal to another source's OSD name does not merge the configured devices.

### Automatic logical-address changes

When one unambiguous native device still matches the same scoped OSD selector but moves from LA 4 to LA 8, the resolver MUST update its runtime binding automatically. Component/Activity records, configured selectors, external slots and both configuration revisions stay unchanged. No activity edit, user confirmation, new Select/Resume intent or replacement workflow is required solely because the LA changed. This preserves the useful name-based behavior already present in `Hub::find()` and `Hub::resolve_device()` while removing the latter's global same-name deduplication. [Baseline name lookup][b-name-lookup] [Baseline name-to-address update][b-name-resolution]

Before accepting the replacement runtime binding, fence queued work against the old binding revision and stop old held-input repeats. Revalidate ownership, scope, domain, role and current observation evidence; then synchronize the incremented runtime binding revision between coordinator and owning actuator before admitting fresh work against the new address. Standalone operation uses the same interface locally; an offline member may collect evidence but cannot grant itself new group execution authority. Do not rewrite an already queued relative command or move a held key onto the new binding. If another device acquires the former LA, neither an old press nor an old cleanup release may be redirected to it. Address resolution itself does not replay routing or input; equipment-state reconciliation remains a planner operation under current intent and ordinary safety rules. The LA change alone must not force blanket standby or require configuration repair. Octopus/coordinator reboot recovery remains the separate lifecycle defined in document 07.

A validator MUST check syntax, identifier form, OSD selector encoding and scope, integer bounds, reference existence, uniqueness, connector limits, and role/resource conflicts before producing an executable configuration. Semantic validation includes one authorized virtual TV per domain, explicit ownership of controlled components, a complete permitted route for each enabled activity, and compatible owner capabilities. It must reject unsupported required capabilities and saved-LA device selectors rather than silently omitting or converting them. A configured name can remain valid while its device is temporarily offline; execution waits for an unambiguous current resolution. Incomplete user drafts may be saved as drafts; they cannot replace the active executable configuration.

Preserve the existing parser's rejection of duplicate keys, unknown fields, non-finite or fractional integers, decoded NUL, trailing text, excessive nesting, and oversize bodies. The current JSON guard has a nesting limit of eight; the schema owner must provide a maximum-depth test vector and either keep the new schema within that limit or deliberately revise the limit and tests. A generic JSON-schema pass does not replace these input-boundary checks. [Input guard][b-json] [Parser tests][b-parser-tests]

The initial serialized configuration payload limit remains 32,768 bytes. A hardware profile may impose a smaller validated limit, advertised before editing/import. Counters above JavaScript's exact numeric range remain decimal strings throughout parsing, UI handling, hashing, export, and peer messages. SHA-256 identifies the exact validated serialized payload bytes; do not hash one pretty-printed representation and distribute another. Canonical serialization and digest test vectors belong with the schema.

### Resumable guided-setup drafts

The coordinator retains one bounded, server-side guided-setup draft per consortium in v1. It records a draft identity/revision, the base configuration/presentation revision pair, referenced discovery snapshot IDs, completed choices and their dependencies, user label overrides, and proposed activity/configuration changes. It references existing records and snapshots instead of copying the entire discovery graph into each answer. The draft is nonsecret: no setup code, QR payload, Wi-Fi password, bearer token, private key or commissioning transcript belongs in it. Enrollment credentials remain with their existing security owner.

Refresh, browser reconnection and coordinator restart recover completed choices. Revalidate referenced evidence on resume; invalidate only the decisions affected by a changed selector, topology, capability or base configuration. An expired discovery snapshot triggers fresh evidence collection, not deletion of every answer. Preserve unaffected choices while reporting stale-edit conflicts. Updates compare the draft revision as well as the applicable configuration/presentation pair, so another tab cannot silently replace completed decisions. Starting setup again resumes the draft or explicitly discards it; it does not create unlimited per-session copies. After successful partial activation, remove the committed proposals and rebase the remaining valid choices onto the new configuration rather than discarding unfinished sibling activities. Retire the draft when all its work is committed or explicitly discarded, using the same interruption-safe record discipline.

The draft is proposed configuration, not another authority: only the coordinator's normal validation/activation transaction can commit it. Suggested activities may accumulate in the draft but cannot execute. Discovery, saving answers and accepting setup suggestions emit no wake, routing or standby commands. **Try activity** is a separate explicit planner intent and is admitted only after the required configuration has activated on its owners.

Draft decisions, proposed activities and any separately staged candidate share the existing 32,768-byte pending-configuration payload budget, or the hardware profile's smaller cap; there is no additional 32 KiB allowance per draft, browser or suggestion list. Account for journal copies, the active configuration, metadata and NVS overhead in the existing partition budget below. Enforce the combined bound before accepting a write, preserve the last valid draft on exhaustion, and expose the capacity result through the normal setup API. Storage qualification must cover the draft together with migration and activation records before enabling resumable setup.

### Portable export and device-bound data

Ordinary export contains schema version, internal record IDs, persistent scoped OSD selectors, zone/activity definitions, topology, capability declarations, quirks, and the source configuration/presentation revision pair as informational provenance. Resolved LAs and live binding authority are not part of the portable selection contract. It contains no Wi-Fi password, setup code/verifier, administration record, TLS key, peer pin, Matter fabric, HomeKit pairing, or BLE bond. Restoring into another consortium is an import that remaps consortium/owner scopes and requires attachment confirmation while preserving the intended OSD selectors; it is not permission to clone security identity. Restoring an unchanged system must resolve those names against fresh discovery rather than replaying old address mappings.

Backup and restore currently exclude device/Matter settings. Preserve that boundary. A separate, explicitly designed encrypted recovery export may later include authority material; ordinary configuration replicas and downloads do not provide recovery authority. [Baseline backup validation][b-parser]

## Persistent record layout

Introduce a versioned storage service rather than extending the current unrestricted string-key wrapper everywhere. Keep keys within the underlying NVS limits and keep ownership explicit.

| Proposed namespace | Contents | Factory reset |
|---|---|---|
| `amity_origin` | Origin-record version, OctopusId, bootstrap state, and optional salt/verifier plus credential generation | Preserve only validated origin records, including an identity whose bootstrap state is `unprovisioned`. |
| `amity_local` | User overrides for tentacle labels and device-local preferences | Erase. Factory hardware profile remains separately provisioned. |
| `amity_group` | Membership, enrolled leaf pins/roles, operational key references, coordinator epoch | Erase. |
| `amity_config` | Active/candidate configuration records, local compiled slices, activation journal, bounded nonsecret setup draft, separate last-requested-intent record | Erase. |
| `amity_export` | Integration gateway's external slot registry and aliases | Erase on device factory reset; preserve through ordinary group configuration updates. |
| `amity_reset` | Reset intent and progress journal | Clear last. |

These names define ownership, not permission to bypass production encrypted-storage requirements. SDK-managed Matter, HomeKit, Wi-Fi and BLE storage retains its established owner. Existing web credentials, hostname, TLS identity and HomeKit aliases in `amity_cfg/amity` need a resumable migration or an explicit legacy reader; do not erase the namespace before all required records have been accounted for.

Persist `last_requested_sessions` in a separate versioned coordinator intent record, not inside the executable configuration or a replayable CEC queue. Retain the requested logical zone/activity and desired modes needed to display or explicitly resume the request. Recovery never restores a live SessionId, PlanId, hold, execution lease or successful equipment outcome from that record. After coordinator restart the planner exposes `suspended_after_restart` until a fresh Select/Resume intent creates a new plan against current observations and configuration. Intent writes use the same interruption-safe record discipline and count toward the storage budget.

The baseline `amity_cfg` partition is 112 KiB. Two maximum-size configurations, peer keys, integration state, migration copies and NVS allocation overhead may not fit. Before enabling grouping, the storage owner MUST demonstrate worst-case occupancy and interruption recovery on the supported partition profile. If the budget fails, reduce the advertised configuration cap or design an explicit partition/provisioning migration; never delete the only valid active record to make room. Followers store their compiled configuration slice and required referenced metadata, not an unnecessary second full consortium graph. [Partition layout][b-partitions]

Persisted compiled slices retain the native name-selection policy. Any separately retained resolved endpoint cache is diagnostic evidence only; it cannot restore an executable native binding after boot without current name resolution and authority synchronization.

### Record integrity and activation marker

Each immutable configuration record has a record-format version, consortium identity, configuration/presentation revision pair, payload length and payload digest. Metadata may be separate from the bounded payload blob, but a committed activation marker must name and hash the complete record including both revisions. The storage layer must not assume a transaction across several independent `nvs_commit()` calls.

Use two generations or an equivalent journaled layout: write the inactive candidate, commit, reread and validate it, then write a small activation marker. The previous valid record remains available until the new marker and record have survived recovery checks. On boot, select the last fully valid committed marker; an unreferenced candidate is staged data, not active configuration. A corrupt committed record causes a diagnosed recovery state, not automatic creation of an empty configuration with the same revision.

## Configuration activation sequence

The coordinator is the only writer of consortium configuration and presentation revisions. An administrator submits `expected_config_revision` and `expected_presentation_revision` with an edit. Either stale precondition returns a conflict and the current pair; the server does not merge topology or security-sensitive ownership silently.

A pure presentation edit validates and journals the complete replacement record, advances only `presentation_revision`, and publishes refreshed UI metadata. It does not reinstall an executable policy, change a session/route generation, revoke an execution lease, cancel a hold, or send CEC. Distributing updated labels to a member is metadata synchronization, not control activation. The following sequence applies to semantic and mixed edits.

1. **Validate:** parse and validate the entire candidate without modifying equipment or active state. Compute affected components, ports, domains, sessions, and owners.
2. **Stage:** compile bounded local policy/configuration slices, reserve storage and runtime capacity, and persist candidates on affected owners using the transport contract. Identify unchanged live owners eligible for the exact-policy rebase below.
3. **Ready:** each required owner confirms the candidate digest, supported capabilities, reserved capacity and applicable binding/domain revisions. An offline required owner prevents activation of the affected change. Unaffected zones can continue operating.
4. **Commit:** durably record the coordinator's new authoritative revision and activation intent. This is the recoverable decision point, not a claim of simultaneous hardware action.
5. **Apply:** each owner atomically switches its in-memory policy/configuration pointer at a local execution boundary. It reports its applied revision. Unchanged live owners acknowledge a rebase to the new semantic revision. Missing acknowledgements leave the change visibly incomplete; no new work is addressed using a revision its owner has not acknowledged.
6. **Reconcile:** the planner handles explicit effects on active sessions. A routing/ownership change cancels stale work and releases matching holds, then follows an explicit transition or reports that user action is needed. A mixed edit does not derive additional CEC effects from its label/order changes.

An unchanged owner's revision rebase is idempotent and permitted only when its exact executable-policy hash and all referenced bindings, roles and revisions equal the committed candidate's corresponding slice. This is not permission to skip semantic validation. Rebase changes the owner's admitted semantic revision without reconfiguring equipment. Queued old-revision operations cannot start a new frame; report their stale result and let the planner decide whether a new operation is justified. An already admitted hold retains its exact target/token and existing local expiry; further positive renewals require the acknowledged new revision and a current execution lease. Matching cleanup releases remain admissible. If an active owner cannot rebase safely, the semantic edit waits for or explicitly quiesces it before commitment. An unchanged offline owner may catch up later but receives no new actuation until synchronization and rebase are acknowledged.

The baseline `Hub::configure()` calls standby when an activity is selected. It MUST NOT be used as the final application path for staged configuration. Replace it with side-effect-free policy installation and separate session reconciliation. Queue reservation before persistence remains valuable, but it is only a local guarantee; it does not prove every consortium member applied the update. [Legacy standby side effect][b-hub-config] [Local commit ordering][b-admission]

If the coordinator crashes before its commit decision, retain the old active revision and discard/expire staged work. After the decision, recover the decision and resume application; do not replay non-idempotent CEC actions. Owners reject work with inconsistent configuration/binding/domain revisions. Source isolation remains active throughout staging, interrupted activation, and reboot.

## Migration from configuration version 1

Migration runs once as a journaled transaction, with a preserved original blob and a downloadable pre-migration backup. It must be repeatable after a power loss without creating fresh IDs on each attempt.

1. Establish or load the persistent origin OctopusId and singleton ConsortiumId. The existing board is octopus-local TentacleId `0`, displayed as connector 1.
2. Allocate stable ActivityIds in original order and persist the mapping before exposing any reordered UI.
3. Convert each referenced legacy device name into a component record that persists a `QualifiedDeviceName` for the unique valid scoped candidate established by the old role and current discovery evidence. Repeated references to the same confirmed scoped device share that record; equal text on different scopes is not a global component merge. Preserve the original matching OSD name and activity references, not its current LA. A later LA change resolves automatically through the same selector. Multiple plausible scopes, duplicate native names within a scope, missing actual OSD evidence and conflicting role evidence remain unresolved with an actionable assignment task. Do not choose the first global match, retain a saved LA, or mistake a legacy LA-derived fallback such as `TV` for a reported OSD name. [Legacy discovery defaults][b-name-resolution]
4. Convert the old optional switch into an explicit capability/route step. Preserve vendor quirks with their scope; expansion to other equipment requires validation.
5. Capture each old external input slot and HomeKit alias/hidden setting against the corresponding new ActivityId before any rename or reorder. See [control surfaces](12-control-surfaces-and-admin.md).
6. Store and validate the schema 2 candidate, activate through the journal, and retain the original until migration has been confirmed healthy.

The front-address field is a fallback setting at this baseline. `determined_front_physical_address` is runtime evidence from the initial back scan, and is frozen during the boot. Migrate the fallback and its provenance, not a supposed permanent identity discovered from one scan. New bindings need the topology checks in [discovery](05-discovery-and-topology.md). [Address behavior][b-pa]

Migration cannot manufacture trustworthy connectivity for a complex installation from old names. Unresolved activities are shown as needing assignment; safely migrated activities remain usable. No factory reset, remote unpairing, Matter fabric removal, or bulk standby is an incidental migration step.

## Bootstrap identity for existing boards

Factory v3 is a 288-byte record whose reserved bytes 226–255 must be zero. Ordinary OTA MUST NOT repurpose those bytes or rewrite the original Matter credentials. A factory v4 image is an explicit provisioning operation with its own parser, test vectors and downgrade policy. [Factory format][b-factory]

For existing hardware, add a separate reset-retained `amity_origin` record. On the first supported boot, automatically create and durably commit a random OctopusId with bootstrap state `unprovisioned` before exposing it to configuration migration or control services. An existing singleton can use that identity immediately after commitment. Identity initialization does not authorize enrollment: enrollment stays disabled while the bootstrap verifier is absent or invalid. If a committed origin record is corrupt, enter identity recovery rather than silently creating a replacement identity.

An authenticated administration session plus physical authorization upgrades the existing origin record with an independent 128-bit consortium setup secret, preserving OctopusId. Persist the versioned salt/verifier and credential generation, deliver the raw code once through the authenticated setup-card flow, and clear temporary copies. The code is not the Matter PIN, HomeKit PIN, or current administration password. Buttonless hardware requires a USB-assisted provisioning procedure; password-only access must not bypass the physical requirement.

Identity initialization and bootstrap provisioning each use candidate/committed records with a final marker. An uncommitted candidate is never externally published; recovery may resume that candidate, and a committed identity is never regenerated on a retry. Enable enrollment only after the verifier-bearing origin upgrade commits. If the owner lost the once-displayed code, use a physically authorized code-rotation transaction that preserves OctopusId and invalidates the old verifier. A factory reset erases membership and operational credentials while retaining the validated origin record and its bootstrap state; rejoining still requires authenticated enrollment. [Enrollment details](09-membership-security-and-enrollment.md)

## Reset and removal semantics

The baseline reset journal lives outside the normal application namespace, clears normal NVS and `amity_cfg/amity`, and clears its marker last. Extend this deliberately for every new namespace. Use an explicit reset-owned namespace list plus a narrow origin-record retention rule; unknown future namespaces must not be silently assumed safe to retain. Test schema upgrades of the reset journal as well as interrupted erases. [Reset implementation][b-reset]

Group removal and factory reset remain different operations. Removal revokes group authority, and the reachable retired octopus becomes passive while inline. It does not automatically form a new active singleton with old CEC claims. Config import never undoes revocation. An unreachable removed octopus cannot be promised remote erasure; surviving peers enforce revocation. [Removal and recovery](13-faults-removal-and-recovery.md)

## Implementation tasks and acceptance

Dependencies: domain/schema contracts, compiled policy interface, coordinator transport, origin/enrollment lifecycle, and integration slot adapter.

| Work item | Acceptance evidence |
|---|---|
| Implement strict schema 2 validator and v1 converter | Malformed/oversize/NUL/duplicate inputs and persisted LA selectors fail without writes; unique legacy names migrate to scoped name selectors; ambiguous cases stay explicit; record IDs/selectors survive interrupted retries. |
| Preserve automatic name-to-address resolution | Same scoped OSD name at a new LA works without config/activity edits; old queued work cannot reach a replacement at the old LA; native/proxy names and duplicated scopes remain distinct. |
| Implement versioned record and activation journal | Power loss after every storage operation yields exactly the prior or committed next revision; no mixed payload/marker is accepted. |
| Add bounded resumable setup draft | Refresh/restart retain completed choices; changed evidence invalidates only affected decisions; combined pending payload stays within its cap; no raw setup secrets or save-triggered actuation. |
| Split configuration installation from activity transitions | Pure rename/reorder changes only presentation revision, preserves holds/leases and causes zero CEC commands; semantic owner rebase rejects stale queued work. |
| Migrate slot registry and aliases | Existing Matter/HAP selectors keep the same ActivityId after reorder, rename, deletion and reboot. |
| Add origin upgrade/reset coverage | v3 bytes remain unchanged; setup material never enters ordinary export/logs; reset preserves origin and removes operational trust. |
| Prove storage budget | Worst-case validated profile retains active/candidate/recovery records through interruption and NVS allocation pressure. |

Storage capacity, ambiguous legacy attachment resolution, and unsupported older firmware are release gates, not exceptions that permit unsafe activation. Full evidence requirements appear in [validation](15-validation-and-release-gates.md).

[b-parser]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/configuration.cpp#L42-L83
[b-store]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/config_store/config_store.cpp#L14-L39
[b-json]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/include/json_input.hpp#L14-L32
[b-parser-tests]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/configuration_test.cpp#L14-L55
[b-partitions]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/partitions.csv
[b-hub-config]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp#L34-L39
[b-admission]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub_service.cpp#L107-L129
[b-pa]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp#L229-L245
[b-name-lookup]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp#L65-L74
[b-name-resolution]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp#L251-L274
[b-factory]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/device_settings/include/factory_credentials.hpp#L15-L74
[b-reset]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/device_settings/factory_reset.cpp#L24-L70
