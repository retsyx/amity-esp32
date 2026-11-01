# Authenticated Amity membership and owner delivery

The consortium is a thin ownership and transport layer around original local Amity. The sole original controller owns UI, remote, integrations, membership and saved configuration. Added devices remain headless and perform original local work for the selected owner/adapter/side/OSD name. [Transport](../../amity-octopus-consortium/08-consortium-transport-and-routing.md), [membership](../../amity-octopus-consortium/09-membership-security-and-enrollment.md) and [AGENTS.md](../../AGENTS.md) apply.

## Operational contract

`scoped_intent` (41) carries the original typed action and qualified target, current generation/sequence, exact key token where applicable and receiver-issued transport authorization. `scoped_result` (42) reports owner-local adapter/device state or the generation/sequence-correlated source-input presence outcome needed to continue the original switch fallback. It does not wait for a CEC ACK. `qualified_lookup` capability 262144 distinguishes this execution contract. Numeric slots of removed operation/hold-renew/binding/plan/profile/config-replication/remote-management features remain retired; unknown/retired kinds reject.

The production closed codec/schema and `wire.hpp` are authoritative for exact payload fields, kinds, capabilities and bounds. Keep current exact-peer TLS, durable role/membership checks, boot/link/configuration/input scope, replay handling and bounded causal IPC. These protect delivery; they are not a persisted HDMI profile or activity availability decision.

The owner resolves equipment using ordinary local discovery and applies original local CEC behavior. The controller does not transmit periodic presses, per-button renewals, a persistent logical address or a replacement opcode policy. Owner-local successful completion paces each held key; other commands submit immediately. No outgoing executor retry queue or global prepared-plan barrier is added.

## Enrollment and role

An original main supports at most three secondary Amities (four devices total).
The membership roster, persistent credential validation and authenticated link
capacity share this limit. Add refuses a fourth secondary before starting a new
scan or committing membership; disconnected saved members still count.

The main’s Amity devices → Add scans directly for unclaimed candidates after the user opens their three-to-below-ten-second FW RESET window. Admission uses fresh ephemeral key exchange and AEAD by default. `CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE`, default off, requires the candidate’s existing printed Matter manual/QR code through PAKE to authenticate fresh Amity session keys. Both ends require the same profile without fallback; no full Matter commissioning or fabric creation occurs. There is no separate Add secret, stored verifier, USB provisioning/rotation step or member relay. Preserve stable origin identity and factory Matter/admin credentials.

Bind the selected candidate and actual hostname, retain exact-leaf LAN proof and commit headless adoption durably. Prepared transactions reconcile idempotently; a lost acknowledgment does not create a new member or undo committed authority.

Only checked committed Matter/network setup or proven retained original authority establishes the main. Reboot and last-fabric removal preserve role. Added devices cannot host UI/home integrations, own a remote, become input gateways, promote/elect themselves or fail over automatically.

## Existing management

Keep authenticated hostname metadata, per-device settings, exact-image peer updates and explicit removal/reset. Settings retain reviewed target boot/settings/presence generations, bounded owner drain, atomic save and selected-device restart after response attempt. Explicit removal retains reviewed membership/target boot, durable revocation/reset journal and actual scheduled/offline/unconfirmed outcomes. Generic loss/retirement does not reset a device.

Normal OTA preserves independent identities/credentials and saved intent. Main replacement uses fresh Matter authority plus existing Advanced restore after old-main isolation; it does not clone keys/pairings/trust or restore activity execution approvals. [Recovery](../../amity-octopus-consortium/13-faults-removal-and-recovery.md) and [updates](../../amity-octopus-consortium/14-ota-and-compatibility.md).

## Evidence

Run the current focused/full suites, including actual codec/session/runtime fixtures, plus baseline all-local/remote frame transcript comparisons. Old graph-model fixtures are not current positive contracts. Host checks do not prove native ESP32 radio, physical CEC/AV, simultaneous adapter load or ecosystem retention; record that evidence separately.
