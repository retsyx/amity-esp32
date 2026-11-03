# Authenticated Amity membership and owner delivery

The consortium is a thin ownership and transport layer around original local Amity. The sole original controller owns UI, remote, integrations, membership and saved configuration. Added devices remain headless and perform original local work for the selected owner/adapter/side/OSD name. [Transport](../../amity-octopus-consortium/08-consortium-transport-and-routing.md), [membership](../../amity-octopus-consortium/09-membership-security-and-enrollment.md) and [AGENTS.md](../../AGENTS.md) apply.

## Operational contract

`scoped_intent` (41) carries original owner delivery and current activity scope. Distribution sends activity selections and key state; each Amity independently runs the original local activity algorithm for its equipment. Lookup and CEC transmission completion order local steps, including local old-only Standby before new Power On. Another Amity's lookup, device state or transmission cannot hold that local job. No per-step CEC orchestration or completion results cross owners. `scoped_result` (42) reports owner-local adapter/device observations for the UI. Raw `key_edges` follow the authenticated stream's ordering without frame IDs, per-key permission tokens or renewable execution grants. The stream supplies original-controller authority and exact boot/configuration scope. `qualified_lookup` capability 262144 distinguishes this execution contract. Numeric slots of removed execution-grant/operation/hold-renew/binding/plan/profile/config-replication/remote-management features remain retired; unknown/retired kinds reject.

The production closed codec/schema and `wire.hpp` are authoritative for exact payload fields, kinds, capabilities and bounds. Keep current exact-peer TLS, durable role/membership checks, boot/link/configuration/input scope and bounded causal IPC. TCP/TLS supplies ordered delivery; no generic application replay cache is added. These protect delivery; they are not a persisted HDMI profile or activity availability decision. Add, Remove, settings and firmware-update operations retain their payload request IDs and explicit idempotence because their results may need reconciliation after a lost response.

The owner resolves equipment using ordinary local discovery and applies original local CEC behavior. The controller does not transmit periodic presses, per-button renewals, a persistent logical address or a replacement opcode policy. The main retains only a raw key's owner index. One receiver table tracks button state and resolves the current activity target again on press, repeat and release. Owner-local successful completion paces each held key; only state edges cross the network and other commands submit immediately. No outgoing executor retry queue or global prepared-plan barrier is added.

Operational TLS sockets require `TCP_NODELAY` so consecutive short TLS records
are sent without waiting for a delayed TCP acknowledgment. Application writes
remain bounded to 512-byte records; framing, authentication, receive/write
deadlines and CEC attempt budgets are unchanged. A native test runs the actual
setup, credential callbacks and framing over loopback TCP, checks the option on
both peers, rejects a socket that cannot enable it, and exchanges frames across
the record boundary in both directions on the same authenticated stream.

An earlier native comparison using the pinned Mbed TLS library and production
methods on Darwin measured 32 transfers of 800-byte bodies: default TCP had a
1 ms median, 2 ms 95th percentile and 3 ms maximum; `TCP_NODELAY` had 0 ms, 1 ms
and 1 ms respectively. The SDK's lwIP delayed-ACK timer can wait 250 ms while
Nagle holds a short following segment. This supports removing transport
coalescing latency; it does not prove that Nagle caused a historical bench key
failure. Native timing does not qualify ESP32 Wi-Fi or physical CEC latency.

Rejected key delivery logs its numeric key and activity generation with the transport,
receive or owner-admission failure stage and result. These diagnostics
retain no payload, credentials, hold tokens or OSD names and do not change
admission, expiry, retries or connection policy.

## Enrollment and role

An original main supports at most three secondary Amities (four devices total).
The membership roster, persistent credential validation and authenticated link
capacity share this limit. Add refuses a fourth secondary before starting a new
scan or committing membership; disconnected saved members still count.

The main’s Amity devices → Add scans directly for unclaimed candidates after the user opens their three-to-below-ten-second FW RESET window. Admission uses fresh ephemeral key exchange and AEAD by default. `CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE`, default off, requires the candidate’s existing printed Matter manual/QR code through PAKE to authenticate fresh Amity session keys. Both ends require the same profile without fallback; no full Matter commissioning or fabric creation occurs. There is no separate Add secret, stored verifier, USB provisioning/rotation step or member relay. Preserve stable origin identity and factory Matter/admin credentials.

Bind the selected candidate and actual hostname, retain exact-leaf LAN proof and commit headless adoption durably. Prepared transactions reconcile idempotently; a lost acknowledgment does not create a new member or undo committed authority.

Only checked committed Matter/network setup or proven retained original authority establishes the main. Reboot and last-fabric removal preserve role. Added devices cannot host UI/home integrations, own a remote, become input gateways, promote/elect themselves or fail over automatically.

## Existing management

Keep authenticated hostname metadata, per-device settings, exact-image peer updates and explicit removal/reset. Settings retain configured-slot, reviewed target boot, settings revision and authority checks, bounded owner drain, atomic save and live owner-local application. A fallback-selected address adopts the edited default; a scanned-selected address is retained, independently of numeric equality. Settings preserve the current boot, authenticated connections and device observations. They require no adapter-presence sensor or responding CEC device. Explicit removal retains reviewed membership/target boot, durable revocation/reset journal and actual scheduled/offline/unconfirmed outcomes. Generic loss/retirement does not reset a device.

Normal OTA preserves independent identities/credentials and saved intent. Main replacement uses fresh Matter authority plus existing Advanced restore after old-main isolation; it does not clone keys/pairings/trust or restore activity execution approvals. [Recovery](../../amity-octopus-consortium/13-faults-removal-and-recovery.md) and [updates](../../amity-octopus-consortium/14-ota-and-compatibility.md).

## Evidence

Run the current focused/full suites, including actual codec/session/runtime fixtures, plus baseline all-local/remote frame transcript comparisons. Old graph-model fixtures are not current positive contracts. Host checks do not prove native ESP32 radio, physical CEC/AV, simultaneous adapter load or ecosystem retention; record that evidence separately.
