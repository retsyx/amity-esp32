# Save user intent and migrate without execution gates

**27 October 2026 user amendment.** Canonical configuration schema 3 stores qualified equipment selections and original activity intent. It removes the former persistent executable topology model. [Addressing](03-domain-model-and-addressing.md), [execution](07-activity-planning-and-execution.md), [original-controller policy](21-original-controller-policy.md) and [AGENTS.md](../AGENTS.md) are normative.

## 1. Persistent configuration

Save stable component/activity IDs, user/default labels, qualified OSD selectors, original display/source/audio/switch selections and switch inputs, activity order, stable home-app slots/aliases and original vendor quirks. A qualified equipment selector is installation scope plus Amity device ID, stable HDMI adapter, front/back side and OSD name. Device hostnames are presentation/network metadata and do not replace stable identity.

Do not save activity `enabled`, route IDs/steps, binding policies, confirmed domains, capability approvals, signal edges, resource graphs or physical-address evidence as execution permission. Do not save current logical addresses, observations, held buttons, valid grants or a current-activity/resume journal. Missing equipment remains a configured selection and uses ordinary live lookup.

## 2. Validation and persistence boundaries

Validate syntax, stable identity form, selector scope, adapter capacity, references, original field bounds and ordering/slot invariants. Validation must not require devices to answer, establish a physical path or support an additional approved capability profile. Separate AVR audio, explicit receiver input and original vendor quirks remain valid configuration.

Retain crash-consistent staging, readback/integrity validation and commit markers, expected revision checks, bounded storage admission and explicit recoverable errors. No owner replica, owner-ready/applied acknowledgment vector or staged-owner digest is stored. The controller saves valid intent independently of offline peers. Its queued owner application reports both configuration and presentation revisions through volatile UI status; enqueue success alone is not an applied acknowledgment. These protect saved intent and concurrent edits; they are not a distributed execution prepare/commit protocol. Configuration and presentation revisions distinguish storage/UI changes; they do not select a different live configure algorithm. Preserve original unconditional configure-to-standby/generation behavior even for label-only edits. Presentation edits cannot rewrite OSD selectors or pairing ownership.

Identity, membership trust, credentials, durable original-controller role, reset journals and hardware/adapter settings remain separate storage. Portable backups do not import old trust, Wi-Fi, private keys, home/remote pairings or another controller's authority.

## 3. Version-1 migration

Keep the exact legacy configuration backup and stable ID mapping needed for restart-safe conversion. Preserve every activity name and role field, optional switch/input, original order, home-app slots, aliases and vendor quirks. Scope the former local selections to the proven original Amity and its existing adapter/CEC side. Resolve any data that the original representation did not identify only through the original local rules; do not guess another owner or silently delete the name.

Migration does not require equipment to be present. Valid original names receive their proven original owner/adapter/side selector independently of current observations. A name that cannot be represented as a legal CEC OSD string remains a placeholder for the original source-absence/switch fallback. Preserve unresolved names as saved intent. Do not disable AVR/switch activities, demand user confirmation of a new topology or limit migrated operation to a direct source/screen path. A component's absence at migration is runtime absence, not a permanent availability decision.

Bind the original controller only from its existing durable authority or checked committed Matter/network enrollment. A portable document cannot establish that authority. Stage/confirm the new boot safely while retaining recovery information, without persisting old runtime addresses or an executable command queue.

## 4. Version-2 migration

Read the prior document's user intent and selectors. Preserve stable IDs, labels, original activity role choices, switch inputs, order, slots/aliases and original quirks. Discard obsolete `enabled` values, route IDs, domains, bindings, resources, capability/signal graph approvals and PA evidence. A formerly disabled activity becomes the same saved activity under live qualified lookup; no enable/repair step is required.

Keep the original backup for diagnosis and storage-cut recovery. Treat old graph data as obsolete input, never as authority to move an owner or choose a new OSD name. Migration must not silently discard the user's unresolved equipment or change their configured behavior. Do not import a persisted activity-resume request.

## 5. Backup, restore and replacement

The ordinary Advanced upload/restore workflow preserves valid component/activity IDs, labels, field values, order and slots. Rebase owner references only to explicitly selected, proven replacement hardware; otherwise retain unresolved references for normal lookup. Restoring configuration does not copy operational keys, remote bonds or home-integration pairings and does not itself start an activity.

A replacement main is established through Matter setup with old-main isolation; the backup cannot create controller authority. Existing members must use the approved physical reset/re-enrollment flow. After identity mapping, activities remain saved choices rather than disabled pending topology approval. Separate AVR/switch behavior is preserved. See [manual recovery](13-faults-removal-and-recovery.md).

## 6. Per-adapter defaults and hardware settings

Preserve the approved atomic per-device settings record and stable adapter slots within the four-adapter profile. The old singleton front-address default migrates only to adapter 1. Each adapter's owner applies its own saved default to the original back-first address selection at boot; it is a fallback setting, not confirmed PA evidence or executable route permission. Keep actual presence generations and selected-owner restart checks for settings changes. CEC silence is not physical detachment.

GPIO mapping, factory commissioning records, reset-retained origin/Add authorization and reset-owned operational trust keep their existing storage contracts. Explicit member removal uses the established durable reset journal and response-attempt boundary. Authority loss and generic fencing do not reset a device.

## 7. Evidence and migration lesson

Counts, labels, a readable document and Ready are insufficient to declare migration parity. Compare execution with the original baseline for AVR audio, explicit switch fallback, quirks, same-target repair, missing equipment and return, and local/remote key lifecycle. Verify no persistent availability or route-approval state remains in canonical serialization. Fault-inject storage cuts and prove user intent, authority and credentials survive without restoring live work.

The previous migration preserved six activities while making four AVR/switch activities non-executable. That was a behavioral regression. Do not instruct the user to repair/reconfigure existing activities to work around such a regression; remove the extra policy and record its actual test evidence.
