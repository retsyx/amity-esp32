# Saved configuration and storage

The current configuration stores qualified equipment selections and original activity intent. [Addressing](03-domain-model-and-addressing.md), [execution](07-activity-planning-and-execution.md), [original-controller policy](21-original-controller-policy.md) and [AGENTS.md](../AGENTS.md) apply.

## Persistent configuration

Save stable component/activity IDs, labels, qualified OSD selectors, original display/source/audio/switch selections and input numbers, activity ordering and stable home-app slots/aliases. A selector is installation scope plus Amity device ID, HDMI adapter, front/back side and OSD name. Hostnames are presentation/network metadata.

Do not persist availability permissions, enabled flags, executable graphs, physical/logical runtime addresses, grants, held buttons or an activity-resume journal. Missing equipment remains selected and uses ordinary owner-local lookup. Preserve separate AVR/audio and explicit switch-input behavior.

## Current format and future changes

Only the current saved format is accepted. Its numeric `version` is checked against `current_configuration_version` (currently 3). Missing, malformed, older and future versions fail explicitly. No older decoder, conversion, compatibility alias, retained migration backup or migration-ID mapping is available. There is no promised compatibility with development snapshots before the first release.

Keep version validation, bounded parsing, explicit errors and the storage transaction boundary as the framework for a future deliberate format change. No automatic migration or format-change workflow runs today. Source types/functions and HTTP routes use versionless names; numeric format metadata remains validated.

Blank storage is initialized directly as an empty current-format configuration using the proven original controller and current hardware capacity. It does not read a name-only configuration or synthesize an old document. An unsupported or corrupt existing record fails closed; it must not be silently replaced by a blank configuration.

## Validation and persistence boundaries

Validate syntax, identities, selector scope, adapter bounds, references, field limits and ordering/slot invariants. Validation never requires equipment to answer or an approved physical path. Retain crash-consistent two-slot staging, readback/integrity validation, commit markers, expected revision checks and bounded storage admission. The current `AMITY3` record and storage-format-2 commit marker remain unchanged; no previous record format is decoded.

The controller saves valid intent independently of offline peers. Owner application reports both revisions through volatile status; enqueue success is not proof of application. Preserve unconditional configure-to-standby/generation behavior even for presentation edits. Presentation changes cannot rewrite OSD selectors or pairing ownership.

A newly staged initial configuration or current hardware-capacity update may remain pending until protected local OTA boot health is confirmed. This is current-format transaction activation, not a format conversion. Keep its authority and flash-safety fences.

## Backup, restore and replacement

The ordinary Advanced workflow accepts only a current-format backup and preserves valid IDs, labels, selectors, role/switch fields, ordering and slots. Portable backups do not import Wi-Fi, private keys, operational trust, remote/home pairings or another controller's authority. Restore does not start an activity. Replacement owner mappings require explicit proven hardware choices; unresolved selections retain normal lookup behavior. See [recovery](13-faults-removal-and-recovery.md).

## Independent device state

Identity, membership trust, credentials, durable original-controller role, reset journals and hardware/adapter settings are separate records with their existing contracts. Preserve stable adapter slots, independently saved defaults and original back-first physical-address selection. CEC silence is not detachment. Explicit member removal retains the durable reset journal and response-attempt boundary; authority loss alone does not reset a device.

## Evidence

Exercise current-format round trips, strict version/obsolete-field rejection, actual blank-storage startup, original authority, qualified selectors and storage cuts. Compare original local and remote AVR/audio, switch fallback, missing equipment/return and key behavior. Counts, labels and Ready alone do not establish behavioral parity.
