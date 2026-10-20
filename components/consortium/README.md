# Authenticated Amity actuator channel

The original main Amity owns configuration, planning, UI, the sole remote and all
home integrations. Added devices are headless actuators. One firmware supports
both roles: fresh boot has no Wi-Fi/UI; checked Matter BLE setup establishes the
main, while authenticated Amity BLE enrollment establishes a member. The durable
role is independent of Matter fabric count. See the [controller contract](../../amity-octopus-consortium/21-original-controller-policy.md).

`runtime_read_startup()` resolves stored role before CEC and user services start.
`runtime_claim_original_controller()` is an application-only durable CAS after
checked Matter/network completion or proven retained main authority. A prepared
member decision wins. Unclaimed/prepared/retired/unknown states cannot issue
positive work. No listener opens before a durable claim or committed member join.
The only retained handling of old transfer state is presence detection that
forces passive recovery; no transfer decoder, successor state machine or API exists.

## Enrollment and shared radio

Register `runtime_register_gatt()` before the shared CHIP/NimBLE host starts.
Physical setup and `radio_control` admit one bounded enrollment workflow. Matter,
the local remote and enrollment never initialize a second BLE host. Members
cancel remote pairing/reconnect/scans/input and disable connections while keeping
unrelated stored bonds; they never pair or forward physical user input.

`runtime_open_enrollment()` requires the admitted physical radio generation.
The original controller's authenticated administration calls
`runtime_add_octopus()` with the candidate's Add code. The `amityc1` code contains
its stable origin identity and an independent secret; it is not a Matter/HomeKit
code, password or URL token. Security2/SRP and encrypted copied-fragment GATT
exchange bind the exact candidate/operational keys. Available network credentials
travel only through the authenticated bootstrap channel. LAN proof and durable
membership commitment precede readiness. An authorized nearby member may relay
opaque bootstrap records, never inspect them or become an input/controller gateway.

Origin/bootstrap credentials are separate from reset-owned operational group
credentials. Physical USB verifier installation/rotation confirms the exact local
identity and atomically verifies the retained record; it neither logs setup secrets
nor clones remote/home pairings. Lost/uncertain writes keep bootstrap admission
closed. See [provisioning](../../tools/consortium_provision.py) and the design's
[enrollment contract](../../amity-octopus-consortium/09-membership-security-and-enrollment.md).

## Operational execution

Operational TLS authenticates current exact leaf pins and role. The closed JSON
registry validates types, bounds, scope, BootIds, epoch/revisions and TTL. No TLS
early-data or stale-link authority is accepted. Snapshot/configuration consumer
application and current owner-state exchange complete before execution leases.

The channel carries configuration, observations, prepared plans/barriers,
controller-originated semantic operations/results, token renew/release cleanup,
bounded scans, authenticated updates and retirement. `system_sync` binds these
to the owner-thread coordinator/LocalCecAgent. The receiving actuator validates
its exact current endpoint/binding/domain/policy/session and inherited deadline
again before queue admission and wire start. It never substitutes a new address
or lifetime into stale work. No raw CEC forwarding exists.

There are no member intent, ingress-lease, remote-management, controller-transfer
or promotion wire kinds/APIs. Unknown/removed kinds reject. Local physical input
is normalized only on the main and preserves its original causal deadline before
planning. An actuator issues receiver-local execution leases only to that main.
Exact-token cleanup and local hold expiry remain independent of a stalled link.

Capability definitions in `wire.hpp` are authoritative. The current known mask
is 126975: the original 1/2/4/8/16 features plus prepared plans 32, snapshots 64,
configuration 128, outcomes 256, peer update 512, sequence 1024, opaque relay
2048, original-controller-only 8192, hostname metadata 16384, per-adapter settings 32768 and factory-reset removal 65536. Removed
owner-remote-management capability 4096 is not implemented. Unknown optional
capabilities are ignored; unknown required capabilities reject. Negotiated
`device_status` (37) reports only the actual canonical hostname. Hello remains
unchanged; older peers retain compatible management/update without receiving
this metadata kind. New induction uses bootstrap profile 2 and binds the
required hostname into its authenticated proposal before Wi-Fi staging.

Resource limits remain explicit: three active devices/two peers, two concurrent
TLS links, ordinary8KiB JSON messages and bounded larger configuration transfer,
finite queues/ledgers, chunk and operation deadlines. Source constants and the
negotiated profile are authoritative; passing a large synthetic topology does
not qualify more hardware. Detailed storage/transport and physical capacity gates
remain in documents [08](../../amity-octopus-consortium/08-consortium-transport-and-routing.md),
[11](../../amity-octopus-consortium/11-config-storage-and-migration.md) and
[15](../../amity-octopus-consortium/15-validation-and-release-gates.md).

## Removal, manual recovery and updates

Removal durably revokes the selected actuator, fences queued work and drains bounded
matching cleanup. A responding supported member then writes its factory-reset journal
before acknowledging reset scheduling; offline or unconfirmed removal requires the
physical reset button and makes no remote-reset claim. Keep old inline hardware isolated before
activating a replacement on its physical domain. Members never resume standalone
UI/control after removal or controller loss.

Main loss has no automatic election, promotion or planned transfer. For manual
recovery, Matter-enroll replacement hardware as a new main and use existing
Advanced upload/restore after old-main isolation. Preserve portable logical
IDs/names/order, import no old trust/keys/leases and leave execution unavailable
until fresh evidence. Re-pair the remote/home integrations. Factory-reset old
members with the existing at-least-ten-second hold, then use the three-to-below-ten
second candidate window to re-add them. Origin ID/Add authorization survives;
operational keys, Wi-Fi and trust are fresh. No identity or bond cloning occurs.

The normal update artifact is `amity-update.bin`: one bounded container with the
exact manifest, optional publisher signature and image. Original-controller
update hooks route bounded chunks to authenticated members with exact release,
job/target/offset correlation, independent target validation and maintenance
fences. Update actuators before the main and require new-boot/image/configuration
health; uncertain installation is not a license to resend blindly. Raw application
images are USB/debug or explicitly pre-staged artifacts. Production signing/native
boot qualification remains separate.

## Validation

Run the current `tests/run_host_tests.sh` for protocol/lifecycle, Security2/GATT,
exact-leaf TLS rejection, queue lifetime, original claim, opaque relay, storage,
update and service/role tests. `consortium_original_claim_test.py` and the actual
main/service lifecycle fixtures cover original-only policy without hardware
mutations. Deleted transfer/member-input/remote-management suites are not current
coverage. Exact software snapshots and remaining physical gates are recorded in
[implementation status](../../IMPLEMENTATION_STATUS.json).

Host/fake-peer tests do not establish native ESP32-to-ESP32 radio interoperability,
in-flight custom GATT continuity, remote/home pairing retention, peer CEC output,
ten-wire timing, AV picture/audio or production security. The independent Pi
fixture's recorded image/topology limits each historical result.

## Per-device adapter settings

Negotiated `settings_read` (38), `settings_write` (39) and `settings_result` (40) use the closed [production schema](protocol-v1.schema.json) and `settings.cpp` codec. Only the authenticated original main requests changes; members stay headless. `RuntimeSettingsHooks` binds the selected owner's snapshot/apply/response-attempt implementation without adding a system-services dependency to this transport.

`runtime_device_settings(owner)` obtains bounded cached/read status; `runtime_write_device_settings(owner, request, core, original_deadline)` admits a typed default-only edit; `runtime_device_settings_operation(owner, request)` exposes its tracked outcome. Settings carry exact BootId, settings revision and each changed stable adapter's presence generation. The owner must be idle, drain wire/cleanup and recheck presence ABA, original deadline and authority immediately before persistence. `runtime_device_settings_authorized(request)` is that final authenticated-authority check inside the owned callback. Unknown/absent/unconfigured adapters are not editable; CEC silence is not physical-presence evidence.

Only durable applied success authorizes the selected device's restart. `runtime_device_settings_response_attempted(owner, request)` schedules it after a response transmission attempt, even when delivery fails, so saved settings cannot strand the owner in maintenance. Mere admission never restarts. A lost result is unknown to the client and is reconciled by a fresh read after reconnect; no automatic write replay. Saved defaults remain separate from detected/selected front addresses and confer no route/proxy authority.

## Explicit member removal and reset

`runtime_remove_octopus(owner, request_id, expected_membership_revision, expected_target_boot)` requires an explicit original-main request and the state reviewed when its confirmation opened. A current live target BootId change or membership revision conflict rejects before roster mutation. A target that became unreachable can be dropped locally, without claiming it reset. The main itself is never a removal target. Known responding firmware without negotiated `factory_reset_removal` support must be updated before removal; no old retirement packet is reinterpreted as permission to erase.

`runtime_removal_operation(owner, request_id)` returns `stage: admitted|complete`, `reset: scheduled|offline|unconfirmed|unsupported` and a message. **Complete means local roster removal.** Scheduled confirms only the member's durable reset-marker acknowledgment, not an observed finished factory reset. Missing/lost acknowledgment requires physical reset instructions; neither polling nor reconnect automatically resends the destructive operation. Pending enrollment cancellation remains distinct and never claims reset scheduling.

The closed membership-event/result kinds 14/15 now use only `phase:factory_reset`, exact mutation/digest/revision and expected target BootId, with results `reset_scheduled|rejected|unknown`. Their short original deadline and digest bind current original-main/member identities and boots, link, group/epoch/configuration and mutation. At the selected member, lifecycle retirement must actually fence and drain the CEC owner before `factory_reset` can durably write the existing reset journal. Revalidate `runtime_removed_device_reset_authorized(authenticated_main)` inside the flash broker immediately before that marker. Only after the result transmission attempt may `after_factory_reset` schedule reboot, even when delivery fails.

The existing factory-reset journal clears Wi-Fi, home fabrics/bonds, operational membership/configuration and per-device preferences while retaining the reset-retained origin/Add authorization. Generic authority loss, link failure, enrollment abort, unsupported messages and ordinary internal passive retirement never invoke it. Offline/unconfirmed removal instructs the user to hold the selected added device's physical button for **at least 10 seconds** before adding it again. All destructive host/browser tests are synthetic; they do not reset a DUT or a paired member.
