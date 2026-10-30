# Original controls and authenticated administration

Only the original controller hosts the browser, sole remote, Matter and HomeKit. Added devices remain headless. Preserve the original forms and input algorithms; [the UI contract](20-user-interface.md), [role policy](21-original-controller-policy.md), [addressing](03-domain-model-and-addressing.md) and [execution](07-activity-planning-and-execution.md) apply.

## Activity controls

Keep Scan HDMI, recommendations, the activity list/reorder, Save/Discard and the existing activity editor fields: Name, Display, Source, Audio, Switch Device and Input. A device choice carries a stable component/qualified selector internally and displays the owning Amity's actual hostname and stable adapter context. Never parse a decorated display string as identity or save a current CEC logical address.

Canonical schema 3 stores original user intent. It has no activity enabled flag, route-approval status, resource/capability graph or physical-path approval. Missing equipment stays saved and is resolved by the selected owner under the original local behavior. AVR audio, explicit receiver input remain supported without new setup questions. Do not ask the user to repair/reapprove an existing activity because transport introduced an execution restriction.

Preserve the baseline effects of configure, received frames, explicit Start, same-target repair, standby and input settling, with the [29 October scan/cache amendment](05-discovery-and-topology.md#2-scan-triggers-and-retained-discovery). Activity saves retain HDMI caches and observations without disconnecting members or implicitly scanning. The existing Scan HDMI control explicitly requests a scan and bypasses lookup cooldown; ordinary control uses owner-local cached recipients or the side-local communication-miss rule. Current request identity/configuration/input/boot checks prevent stale mutations; they do not introduce prepared execution plans. Keep actual admission/completion/error outcomes distinct. Do not persist a current-activity/resume journal or replay missed input after reconnect.

## Remote and integrations

The sole remote remains paired to the original controller. Preserve its original decoding, independent key lifecycle, wait-for-release gate, Siri macros and swipes. Deliver generated state changes/intents to the equipment owner; completion-paced repetition and CEC decisions run there. Added devices cannot accept remote input or host a fallback control surface.

Retain in-place Matter/HomeKit endpoint/accessory identities and pairings. Existing stable ActivityIds, external slots, aliases and hidden/tombstone choices remain presentation/selection records. Availability is runtime lookup, not a persisted enabled value. Slot selection invokes the same original activity behavior. Additional external accessories/endpoints require their own separately requested compatibility work.

## Browser and management

Use the approved tabs: Activities, Remote, Matter, HomeKit, Amity devices, Advanced. Preserve existing login, password/hostname, diagnostics, logs, firmware, backup/restore, restart and reset workflows. Browser bearer sessions stay on the original controller and are never sent to peers. Keep HTTPS/CSP, strict JSON/bounds, authentication and no-secret-cache rules. Browser certificate trust is separate from peer trust.

The Amity devices tab lists the main first as `<hostname> (this device)` with Settings in the same title row. Added devices have actual hostnames, Settings and appropriate Remove/Replace actions; their firmware updates go through the main. No main Remove/Replace action, independent Location alias, member UI, remote-owner mapping or primary-zone chooser is added.

Add scans directly from the main for unclaimed candidates with physically opened setup windows. The user chooses a candidate if several appear, supplies a valid unique Name, and adds it. Only the stricter matched build profile asks for the candidate’s existing printed Matter manual code or QR payload. The required name becomes its persistent lowercase hostname through the encrypted admission session. Renaming a network endpoint does not change the stable device ID or equipment selections. An unavailable device name displays Name unavailable; cosmetic aliases cannot rename hardware.

## Administration data

The production HTTP handlers own exact paths, closed bodies and limits. `/api/config` returns schema-3 saved intent without runtime logical addresses or secrets. The existing activity-editor adapter preserves the baseline form while carrying stable IDs, scoped choices and applicable revision/request context. Semantic selector changes and labels have distinct storage revision metadata; every applied configuration still follows the original configure-to-standby/generation behavior. A save does not become a topology-approval transaction.

Management requests retain their existing target identity, reviewed boot/revision, bounded request/result correlation and explicit destructive-action confirmation. A transport/IPC receipt is not CEC admission or completed hardware work. Unknown results require fresh inspection; do not automatically repeat uncertain relative or destructive work.

## Per-device settings contract

`GET /api/devices/<OctopusId>/settings` is non-actuating. Return status plus a settings snapshot when available. A snapshot contains `boot_id`, decimal `settings_revision` and stable `adapters`. Adapter records contain `tentacle_id`, display `name`, `configured`, `presence`, decimal `presence_generation`, saved default, observed front PA and effective front PA. Preserve advertised one- or four-adapter capacity, including holes. Unknown/absent/unconfigured/offline/unsupported states are read-only; CEC silence does not establish presence.

`POST` to that path retains `{request_id, expected_boot_id, expected_settings_revision, confirm_restart:true, changes}`. Each change carries its stable adapter ID, expected presence generation and requested default front PA. Empty resets only that slot to 1.0.0.0. Validate hierarchical PA, bounds, unique adapters and exact owner before effects. Reserve/drain the selected owner and recheck current presence/authority immediately before atomic persistence.

A started write returns its actual durable result even across the original deadline. Durable success authorizes only the selected device's restart after the response attempt, including lost delivery; admission alone does not restart. No automatic replay after uncertainty. Saved defaults are ordinary startup settings, not approved-topology evidence. [Settings schema](schemas/adapter-settings.schema.json).

## Removal and recovery

Remove reviews the target ID, configuration/presentation and membership revisions, current target BootId and exact affected activity IDs. The confirmation lists those activities and warns they will be deleted. The original main deletes the device's owner/equipment/settings/name and all referencing activities, revokes membership, fences/drains its work and asks a responding supported member to commit its reset journal. Complete requires applied configuration cleanup and ordinary metadata deletion as well as roster removal; scheduled separately requires durable target acknowledgment and does not prove erasure finished. Offline/unconfirmed removal requires the physical reset instructions. Existing saved-only device records support the same deletion. Generic link loss, retirement or enrollment cancellation does not factory-reset anything. Replace remains available before Remove, preserving activities through reviewed mappings while revoking and forgetting the old hardware.

Restore uses the existing Advanced workflow and saves original activity selectors/choices. A replacement main must have proven new authority with the old main isolated; backups do not clone identity, keys or pairings. Explicit owner mappings replace selected hardware IDs; unresolved names remain saved without disablement or a new physical-path approval. See [recovery](13-faults-removal-and-recovery.md).

## Evidence

Browser fixtures must verify original field/order/workflow behavior with local and remote qualified choices, AVR audio/switch fields, retained missing choices and no enabled/approved-path UI. Service tests must verify real original algorithm dispatch and backend stale/auth rejection. A screenshot or retained JSON fields alone does not establish CEC parity.
