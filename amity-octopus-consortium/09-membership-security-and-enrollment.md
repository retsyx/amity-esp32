# Identity, membership and authorized enrollment

Membership protects who may deliver work to an owner. It does not approve HDMI routes or replace local Amity algorithms. [Transport](08-consortium-transport-and-routing.md), [shared radio/setup](10-ble-and-matter-coexistence.md), [storage](11-config-storage-and-migration.md) and [original-controller policy](21-original-controller-policy.md) apply.

## Separate identities and credentials

- Stable random Amity device ID identifies hardware independently of MAC, hostname, IP address or Matter identity.
- Installation ID and the durable original-controller/member role identify the current system authority.
- Reset-retained origin and Add authorization remain separate from reset-owned operational group credentials.
- The Add to Amity code is separate from Matter/HomeKit setup codes, web admin password and remote bonds. The current physical USB tool writes a private `amityc1:<device-id>:<secret>` artifact and installs only its salt/SRP verifier on the device; it does not retain a recoverable raw code there. Follow the actual tool/codec bounds rather than inventing a second code format.
- Operational P-256 private keys and exact peer leaf pins are separate from browser HTTPS keys and release-signing keys. Private keys do not leave their device.

An existing board with no Add verifier cannot be enrolled until it is physically provisioned. Lost/disclosed Add authorization uses the explicit identity-confirmed physical rotation path. Retain existing stable origin identity and independent Matter/admin credentials. Do not silently use reserved factory bytes, reset pairings or generate a new device identity to repair a missing setup code.

## Original-controller authority

The first successful Matter setup establishes the original controller only after committed fabric, fully disarmed fail-safe and checked persistent connected Wi-Fi. Proven retained original authority can recover the same role on upgrade. Merely receiving Wi-Fi, seeing a fabric arrive or reading a portable config file cannot create authority. The durable role survives removing the last fabric and ordinary reboot.

An added device commits its headless member role before adoption acknowledgment. It cannot host UI/home integrations, own a remote, promote itself or become an input gateway. Stale or competing setup cannot change a committed role. Ambiguous/corrupt identity or membership remains passive for physical recovery rather than guessing a new authority.

## Physical setup and naming

Use the existing bounded setup gestures and arbiter. A fresh eligible main uses Matter BLE; a candidate uses its separate Add code and physically opened Amity enrollment window. An administrator password alone cannot replace physical candidate authorization. The existing three-to-below-ten-second hold opens candidate setup; a ten-second hold performs factory reset. Buttonless hardware uses the approved explicit USB path.

Add requires a valid unique Name before radio work. Normalize the 1–32-character DNS hostname label to lowercase after trimming surrounding whitespace; reject internal spaces/punctuation and leading/trailing hyphens. Bootstrap profile 2 binds that actual persistent candidate hostname to the authenticated proposal. Naming does not change device identity or saved equipment selectors. A legacy cosmetic location alias cannot rename hardware on upgrade.

Transfer the system's available supported Wi-Fi credentials only through authenticated bootstrap when needed. Preserve an already configured candidate network. Do not expose network credentials or raw setup secrets to the browser, relay, URLs, logs, analytics or third-party QR services.

## Durable enrollment transaction

Keep the existing bounded single-candidate transaction and canonical request/digest correlation:

1. Authenticate both Security2 SRP/AEAD endpoints; reject wrong proofs and unsupported profiles without insecure fallback.
2. Bind candidate/controller IDs, installation, proposed headless role, join identity/digest, membership revision, exact operational leaves/pins and required canonical hostname.
3. Persist prepared records; the candidate commits/validates the agreed hostname before network staging and acknowledgment. Provisional trust grants setup/status only.
4. Commit network state through the existing Wi-Fi owner, then prove the prepared exact-pin operational LAN path with a fresh challenge.
5. Commit membership durably and idempotently on both sides before reporting ready. A lost acknowledgment reconciles the same transaction; it does not allocate a second member or revert committed authority.
6. Close the setup advertisement and erase ephemeral secrets. Retain only necessary membership/identity/transaction records, not browser setup-code copies.

Restart/cancel/status follows the actual durable pre- or post-commit state. A pre-commit abort removes provisional trust; post-commit cancellation is explicit removal. A prepared pinned LAN transaction may resume without reopening BLE or saving the raw code. A new bootstrap session still needs physical authorization.

## Operational delivery and relays

Exact-peer TLS and current original-controller role authorize typed owner delivery. Reject wrong installation/owner, stale boot/link/configuration/input context, replay and malformed messages. These fences cannot become a stored activity-enabled/profile/route approval. Owners use current local equipment lookup and original algorithms.

An authorized nearby helper may relay opaque bootstrap records under the existing bounded reservation. PAKE endpoints remain controller and candidate; the helper sees neither Add secret nor plaintext Wi-Fi credentials and gains no control/input authority. No generic peer-to-peer command overlay is added.

## Removal and qualification

Explicit removal revokes the selected member and follows the established drain/reset-journal/result-attempt flow. Generic link/authority loss and setup abort do not erase devices. Origin/Add authorization remains reset-retained; operational trust/network/roles are reset-owned. [Recovery](13-faults-removal-and-recovery.md).

Keep protocol/parser, wrong-proof, exact-pin rejection, storage-cut, late-callback and concurrent-role tests. Physical radio interoperability, credential protection and production secure boot/storage require separate evidence. Authenticated peer reports are not hardware attestation or proof that HDMI equipment responded.
