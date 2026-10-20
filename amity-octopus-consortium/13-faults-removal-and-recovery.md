# 13. Faults, removal, and recovery

## Purpose and governing rules

This is the proposed v0.1 lifecycle contract for OCTOPUSES in a CONSORTIUM. It distinguishes loss of connectivity, intentional retirement, hardware replacement, planned authority transfer, and recovery after authority loss. None of these distributed lifecycle features is implemented by the baseline Amity commit `d50af8026938797fc0820d0107f4c4e064c2d2bc`.

The coordinator is fixed in v1. A timeout does not elect another coordinator, authorize a member to self-promote, or turn an inline member into an independent activity controller. Configuration replicas help reconstruct the system; they are not credentials to take it over. [09](09-membership-security-and-enrollment.md) defines the membership trust relationship, and [08](08-consortium-transport-and-routing.md) defines freshness, acknowledgment, and execution leases.

Local CEC isolation is independent of every network lifecycle. An offline, retired, or recovering OCTOPUS must continue preventing an isolated source's unauthorized activation/routing traffic from crossing its tentacle. A software failure must not connect the two CEC wires electrically. Hardware and semantic policy requirements are in [04](04-hardware-and-cec-runtime.md) and [06](06-cec-roles-and-message-policy.md).

[User experience and guided setup](20-user-experience-and-guided-setup.md) owns the visible lifecycle: retain offline equipment, preview the actual effects of removal once, start replacement from the old location and preserve completed setup choices. Normal LA churn must not open a repair wizard. Actual identity or attachment changes receive a focused mapping step. Cosmetic location changes do not move the administration hostname or TLS origin. The nonsecret setup draft resumes user choices; it is not authority to replay an uncertain command or bypass the recovery rules below.

## Runtime state and loss of connectivity

| State | Authority and permitted behavior |
| --- | --- |
| Active and synchronized | Execute only work admitted by current configuration, coordinator epoch, authenticated connection, and receiver-issued execution lease. |
| Disconnected or coordinator unavailable | Preserve membership and source isolation; cancel remote holds and stop admitting new distributed actions. Keep already established equipment state unless a local safety policy requires an action. |
| Reconnecting | Authenticate pins, exchange current BootIds, reconcile configuration/observations, then obtain fresh leases only for eligible sessions. A TCP connection alone does not restore authority. |
| Enrollment pending | Permit only the bounded enrollment/status behavior in [09](09-membership-security-and-enrollment.md). |
| Retired | Remain isolated and passive; no new proxy claims, activity control, or automatic standalone operation. Reclaim requires physical authorization. |
| Recovery required | Reject distributed execution and expose diagnostic/recovery status. Never guess which saved authority is current. |

On connection loss, cancel held keys promptly. The initial local hold policy is a 750 ms lease renewed at most every 250 ms, with no lease beyond 2 seconds; these are validation targets, not measured baseline timings. A release/cancel path bypasses ordinary queue overload and applies only to its matching hold token. Do not release a newer owner's key hold because an old connection disappeared.

Transport TTL alone is insufficient: a delayed command might otherwise receive a fresh deadline on arrival. Require the receiver-issued execution lease from [08](08-consortium-transport-and-routing.md), bound to the current link, both BootIds, coordinator epoch, configuration, and a receiver-local deadline. Revalidate before the first CEC transmission, not merely at network receipt. Do not compare independent monotonic clocks. A command arriving after its lease expires is stale even if its own `ttl_ms` is large.

Apply the same principle before planning: member intents and positive hold renewals need the coordinator-issued ingress lease defined in [08](08-consortium-transport-and-routing.md). Otherwise an old delayed intent could produce a fresh operation. Real-time input/execution uses the shorter lease class; ordinary planning uses its separately qualified class. Exact-token release/cancel remains a cleanup exception and must not wait for a fresh ingress lease. Reconnecting drops old input queues as well as old operation queues.

Reconnection follows [07](07-activity-planning-and-execution.md)'s explicit resumption policy. Persisted `last_requested_sessions` support the UI and Resume action; after coordinator reboot they enter `suspended_after_restart`. Synchronize and observe without automatically waking, routing, or putting equipment in standby. A new Select/Resume intent creates a new SessionId/PlanId after validation. A required member reboot degrades its affected sessions and requires an explicit reconciliation intent; unaffected sessions continue. An ordinary same-boot reconnect may restore fresh-input eligibility after validating current resolved bindings for the same persisted `QualifiedDeviceName` selectors, compatible policy and a sufficiently known route. Automatic unambiguous LA-only re-resolution may advance binding revisions; it does not itself require user confirmation or Select/Resume. Old work remains fenced. Reconnection never replays relative inputs or missed routing steps; independent route uncertainty requires a new Select/Resume intent.

If a node crashed after transmitting a CEC command but before recording its result, the outcome is **unknown**; an acknowledgment journal cannot promise exactly-once execution on external equipment. Query state where the device supports it, or show the uncertainty. Do not repeat a power toggle, key press, or input cycle merely because a network acknowledgment was lost.

Ordinary home-control or BLE input received while its required coordinator is unavailable returns an unavailable result or a clearly bounded local diagnostic response. It does not silently become a second session planner. Local handling still releases keys and enforces previously installed isolation rules.

## Intentional removal

The administrator selects **Remove Octopus** on the coordinator. Show the affected tentacles, activities, and active sessions before applying the change. Offline status alone never invokes this operation.

The removal transaction has these ordered responsibilities:

1. Allocate a membership mutation ID and persist a newer membership revision marking the member revoked. Remove its ability to originate input/observations, receive new work, or serve as an enrollment relay. Absence from the active allowlist must deny authority even if a diagnostic tombstone has been pruned.
2. Cancel dependent plans and coordinator-side holds, invalidate relevant route/configuration generations, and stop requesting or using that member's receiver-issued execution leases. Closing its operational link invalidates those leases at the receiver; local expiry is the fallback for an unobserved network failure. Other owners reject work whose now-invalid resource/binding assumptions depend on it.
3. If an authenticated connection exists, send a best-effort retirement notice over that already established connection. After local revocation, the connection may finish only the retirement/status exchange, not ordinary application messages. Then close it. A disconnected member cannot establish a new active session using its revoked credential.
4. A reachable recipient persists `RETIRED` before acknowledging. It releases its matching holds, stops accepting distributed control, retires its proxy roles safely, and clears consortium credentials/configuration authority as specified by the storage transaction. CEC isolation remains; it does not enable a transparent bypass.
5. Keep portable topology references visibly unresolved until the owner deletes or replaces them. Do not silently assign another similarly named source or an OCTOPUS with the same IP address.

There is no shared group private key to rotate when an ordinary member is removed. Its unique credential is simply no longer authorized by the coordinator. Any future credential changes use authenticated pending/commit transitions; no live connection may adopt a different pin because discovery announces one.

Removal is not a remote erase guarantee. An offline member may retain its Wi-Fi password, old public roster, and last local CEC role assignment. Its execution leases expire locally, and the coordinator rejects it on return, but the administrator cannot remotely erase a disconnected device or instantly remove an electrical CEC address it still claims. Before activating a replacement on the same physical domain, disconnect or positively retire the old hardware. If Wi-Fi knowledge must be revoked, change the network credential through the appropriate network administration process.

V1's coordinator-only actuation path limits revocation exposure: other members do not trust the removed member to command them. A future direct peer overlay would need an explicit stale-roster policy and cannot promise immediate revocation during a partition. This limitation is a reason not to invent all-to-all trust in the first release.

**Remove from consortium** is distinct from deleting Matter fabrics, removing a HomeKit pairing, unpairing a remote, or factory-resetting a device. Keep these actions and their consequences separate in [12](12-control-surfaces-and-admin.md). The baseline already separates Matter fabric removal and per-remote unpairing; preserve that separation. See pinned [Matter administration](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_matter/amity_matter.cpp) and [remote bond handling](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/ble_remote/ble_remote.cpp).

## Replacement and recabling

**Replace Octopus** should preserve the owner's logical system while assigning fresh hardware identity. Revoke the old member, enroll the new board with its own `OctopusId`, operational key, and setup code, and map the old attachment roles to explicit new `TentacleId`/side endpoints. Preserve persistent `ComponentId` and `ActivityId` only where the owner confirms the equipment is the same. Never clone a private key, BLE bond database, Matter Node ID, or HomeKit identity to make replacement appear transparent.

That approved mapping rewrites the affected qualified OSD-name prefixes to the new octopus/tentacle/side while retaining the selected OSD names where verified. Clear old resolved-address caches and rediscover current LAs. Board replacement is a semantic attachment change; ordinary address churn on the existing attachment is automatic runtime resolution and must not enter this replacement workflow.

The candidate starts quarantined for routing until its board profile, port orientation, discovered devices, domain membership, and proxy identity capacity have been validated. A changed cable can merge previously independent CEC domains. That may put a real TV at logical address 0 beside a proposed virtual-TV role, or consume the last usable playback address. Quiet traffic is not proof that the domains are separate.

Stage one configuration/binding transaction, then activate its new revision after all affected owners acknowledge readiness. Invalidate old binding/domain revisions and execution leases. An unavailable old owner is not permission to reuse its electrical role while it is still attached. Tests must include replacement with swapped front/back wiring and accidental domain merging; see [05](05-discovery-and-topology.md).

Replacement does not automatically transfer the gateway that external home apps paired with. If the old board was the Matter/HomeKit gateway, the owner may need to re-pair integrations separately. Stable activity slots and configuration import help preserve intent, but a hostname or consortium epoch does not migrate an ecosystem's cryptographic identity.

## Planned coordinator transfer

V1 offers an explicit **Move coordinator** operation rather than automatic failover. The current coordinator must be healthy enough to authorize and journal the transfer. Every retained member must be reachable; the owner must explicitly remove an offline member or defer the operation. This conservative rule avoids promising a safe transfer across an unresolved partition.

Use one `handover_id` and an immutable proposal containing consortium ID, predecessor/successor IDs and leaf pins/certificates, old/new coordinator epochs, retained membership revision/list, committed configuration revision/digest, and the intended successor role. The new epoch is strictly greater than the old one; encode revisions as canonical decimal strings as required by [08](08-consortium-transport-and-routing.md).

1. **Preflight and quiesce.** Verify the successor's supported firmware/profile, storage and memory capacity, complete configuration, and current operational key. Stop new distributed plans, release holds, and wait for in-flight work to finish or become explicitly unknown. Transfer public member certificates/pins and role metadata to the successor over the authenticated channel; never export any private key.
2. **Prepare all retained members.** Send the proposal through the current authority's authenticated channels. Each member durably stores `HANDOVER_PREPARED` and acknowledges. Prepared members execute neither old nor new coordinator commands; existing local source isolation continues. They retain a restricted old-authority channel only for the handover/status protocol.
3. **Fence the predecessor durably.** Only after all prepare acknowledgments, the old coordinator persists `COORDINATOR_FENCED` with the handover digest. It can no longer request/use execution leases or submit actuation as the old authority, including after reboot. Its remaining authority is to finish this exact transfer.
4. **Commit the successor.** The predecessor sends the commit over the already authenticated restricted channels. Each recipient atomically commits the new coordinator ID/certificate/pin and epoch, closes its old operational channel, and connects to the successor. The successor persists its committed coordinator role before accepting ordinary membership administration. It waits for all retained members to confirm the committed transfer and synchronize before enabling execution.
5. **Finish and reconcile.** The predecessor becomes an ordinary member if retained. Fresh Hello exchanges carrying current BootIds, configuration synchronization, and execution leases establish runtime readiness. Reconcile observed equipment state under [07](07-activity-planning-and-execution.md)'s resumption policy instead of replaying the pre-transfer plan queue.

These are journaled steps, not an atomic distributed write. The predecessor can crash after fencing itself but before delivering every commit; on reboot it resumes the same restricted transfer. A member that only received Prepare remains safely paused until it receives the matching Commit. Duplicate messages repeat the recorded result. A proposal with the same handover ID but a different digest is rejected.

Use `membership_event` for authority-issued retirement/handover phases and `membership_result` for the corresponding durable acknowledgments, following [08](08-consortium-transport-and-routing.md). Payloads carry the mutation/handover ID, proposal digest, closed phase/result enums, membership revision, and committed coordinator epoch where applicable. A final acknowledgment may describe the newly committed epoch over the existing restricted predecessor link; this is an exact-transaction exception, never permission for old-epoch actuation. After closing that link, a lost acknowledgment is resolved through the successor's authenticated synchronization/status. No generic old-authority trust is restored to obtain it.

After Prepare has been durably acknowledged, v1 does not provide an automatic timeout rollback to the old epoch. A lost predecessor can leave the system paused and require the physical recovery procedure below. Do not clear fencing records to improve availability. This intentional limitation makes the first implementation testable without a consensus protocol.

Moving coordinator authority does not require moving the external Matter/HomeKit gateway. A retained former coordinator can continue forwarding authorized external intents to the new coordinator. Its ecosystem pairings remain on that hardware. Moving or replacing that gateway is a separately visible integration operation.

## Unplanned loss of the coordinator

Members must not infer authority from “highest uptime,” “lowest MAC,” an mDNS hostname, a copied configuration file, or a larger self-announced epoch. A malicious device can claim all of those. The ordinary local replica has neither a group CA private key nor a hidden recovery signing key.

The v1 recovery path is deliberately physical:

1. Identify and physically isolate the unavailable/stale coordinator from the control network and any conflicting CEC roles before rebuilding. If it later returns, it must not re-enter using its old authority.
2. Select a replacement authority through its authenticated local administration or USB-assisted setup and physical authorization. Create a new `ConsortiumId`; import a verified nonsecret configuration snapshot as a draft, remapping its consortium-scoped addresses. Preserve logical activity/component identities and external slot mappings where appropriate.
3. Physically reclaim each retained OCTOPUS and re-enroll it using [09](09-membership-security-and-enrollment.md). Retain its origin `OctopusId`, rotate its operational credential, erase old membership trust, and install only the new coordinator's credential. A membership-reclaim operation may preserve unrelated Wi-Fi, Matter, HomeKit, and remote pairing state; it is distinct from factory reset and must have an explicit implementation/UI path.
4. Validate topology and resource ownership before activating routes. Previously established equipment states are observations to reconcile, not a queue of commands to replay.
5. Treat the old coordinator as removed hardware. Its former credentials and group ID are rejected by the rebuilt consortium. Reuse requires the same physical reclaim/enrollment process.

Recovery can be easier in a later release using a separately designed encrypted authority backup, but v1 does not export an authority secret in an ordinary configuration download. Such a future feature must define encryption, custody, rollback prevention, stale-primary fencing, and which identities it can actually restore. Do not label a configuration replica a seamless failover mechanism.

## Reset, bootstrap loss, and credential changes

Factory reset must include every new membership, operational-key, prepared-join, handover, transport-ticket, and configuration-authority record in the durable reset plan. Retain the immutable factory setup record and the deliberately reset-retained `amity_origin` identity/verifier. The baseline reset mechanism already journals cleanup and performs restart-time recovery; extend it rather than adding an unrelated best-effort erase loop. See pinned [factory-reset implementation](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/device_settings/factory_reset.cpp) and [settings persistence](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/device_settings/device_settings.cpp).

A reset inline board starts in a safe unclaimed configuration. It must not immediately claim a virtual-TV identity, execute a saved route from an obsolete snapshot, or announce itself as a new standalone controller. Explicit local configuration/enrollment activates those roles. Membership reset does not erase the board's physical isolation boundary.

The printed bootstrap code may remain valid across reset because its verifier is retained, but it still requires a new physical enrollment window. Loss/disclosure of the code uses physically authorized verifier rotation; changing an administrator password or Matter PIN does not rotate it. Buttonless hardware uses the documented USB-assisted path rather than bypassing presence checks.

An ordinary operational credential update must prove possession of the new key, prepare the new certificate/pin durably, establish a new authenticated connection, and commit before retiring the old credential. Bound any overlap to that transaction and reject both old-session actions and late update messages after completion. V1 may expose this as re-enrollment if an in-place update is not implemented. Changing the coordinator credential requires the planned authority-transfer/fencing discipline even when the successor is the same physical MCU; silently replacing its web certificate is unrelated.

## Required failure tests and implementation dependencies

Integrate this lifecycle with MembershipStore, GroupService/SystemControl, the reset journal, local policy snapshots, and configuration revisions. No individual component may “repair” a missing record by creating a new authority automatically. Invalid/corrupt authority state enters recovery-required mode with a diagnostic reason.

Acceptance tests must cut power before and after every enrollment, removal, and handover commit; lose/duplicate/reorder every acknowledgment; hold a member offline throughout removal; restore an old coordinator after recovery; and restart a prepared member with no reliable wall clock. Assert that at most one committed coordinator can issue accepted work and that stale execution leases cannot revive an old route. Exercise unknown CEC outcomes, key-release deadlines under queue pressure, replacement while old hardware remains attached, domain merges, and reset halfway through namespace erasure. Use independently clocked simulated OCTOPUSES as well as hardware tests; a singleton fake worker cannot demonstrate these properties. [15](15-validation-and-release-gates.md) owns the complete validation matrix.
