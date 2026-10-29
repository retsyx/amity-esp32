# Identity, membership and adding a device

Membership protects who may deliver work to an owner. It does not approve HDMI routes or replace local Amity algorithms. [Transport](08-consortium-transport-and-routing.md), [shared radio/setup](10-ble-and-matter-coexistence.md), [storage](11-config-storage-and-migration.md) and [original-controller policy](21-original-controller-policy.md) apply.

## Identity and credentials

Stable random Amity device IDs identify hardware independently of MAC, hostname, IP address and Matter identity. Installation identity, durable original-controller/member roles, operational private keys and exact peer leaf pins retain their separate purposes. Private keys stay on their device. Origin IDs and factory Matter/admin credentials survive the ordinary factory-reset contract; operational group trust/network/roles are reset-owned.

There is no separate Add secret, stored bootstrap verifier or USB Add-code provisioning/rotation step. Startup discards the obsolete legacy Add-verifier record without loading it; an erasure failure is a warning, never an admission gate, and does not change origin or factory credentials. Adding a device does not require restoring old provisioning artifacts. The factory Matter code remains available for ordinary Matter setup and the optional stricter Amity admission profile.

## Physical workflow

At boot, automatically open the bounded Matter setup window when the current fabric count is zero and the board is either unclaimed or the proven original main with retained authority. This includes factory-reset and still-unclaimed reboots and requires no FW RESET GPIO. It is a permanent user requirement, with no commissioning-history latch or saved-activity/configuration gate. A main whose last fabric was removed keeps its main role/UI and reopens setup at boot. Boards with a fabric, and headless/prepared/retired/recovery roles without one, do not open it automatically. The original main also retains explicit Matter pairing controls.

On the original main, open **Amity devices → Add** to scan directly for nearby unclaimed boards. Hold the candidate's **FW RESET** for at least three seconds and release before ten seconds. Choose the candidate if several are found, enter a valid unique **Name**, then add it. The candidate must remain within the existing bounded physical setup window. A ten-second hold retains its factory-reset meaning; the shorter setup action must not fire first.

Both hold thresholds use the same accepted physical press edge. Debounce confirms
that edge without shifting the hold clock, and the release edge stops the clock
before release debounce. A 9.999-second release opens Add; a 10-second release or
continued hold resets once. `tests/consortium_test.cpp` covers these boundaries,
delayed press confirmation and one-shot reset; the actual setup/reset GPIO task
in `tests/reset_button_interrupt_test.cpp` covers releases before, at and after
ten seconds, press/release bounce and boot-held suppression. This explicitly
aligns the setup gesture's reset threshold with its existing physical Add clock;
the baseline's separate debounced reset clock no longer extends the Add range.

Normalize names to lowercase after trimming surrounding whitespace: 1–32 letters, digits or hyphens, with no leading/trailing hyphen. Bind the selected candidate ID and proposed hostname into admission before applying credentials or membership. A hostname is metadata, never authority or equipment identity. Do not turn old cosmetic labels into hostnames during migration.

## Admission profiles

The normal build uses fresh ephemeral key exchange and AEAD-protected messages during the physically authorized window. Sensitive Wi-Fi and membership records travel only after the encrypted session is established.

`CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE`, default off, selects the stricter build. The user enters the candidate's existing printed Matter manual setup code or QR payload; PAKE proves code possession and authenticates the fresh Amity admission session keys. It does not run full Matter commissioning, add a fabric or create an original-controller role through Matter.

Both main and candidate must use the same profile. Bind the profile and participants into the exchange and reject profile mismatch, malformed records, bad proofs and replay without downgrade or fallback. Keep setup codes and derived secrets transient; do not write them to browser storage, logs or portable configuration.

## Commit the existing membership transaction

After the selected profile establishes the encrypted session, retain the existing bounded enrollment transaction:

1. Bind candidate/controller IDs, installation, proposed headless role, join identity/digest, membership revision, operational leaves/pins and canonical hostname.
2. Persist prepared records and commit/validate the candidate hostname before network staging and acknowledgment. Seal candidate advertising once prepared, while the same encrypted connection may finish network transfer. Provisional trust grants setup/status only.
3. Commit supported network credentials through the existing CHIP Wi-Fi owner when needed, then prove the prepared exact-pin LAN path with a fresh challenge.
4. Commit membership durably and idempotently on both sides before reporting ready. A lost acknowledgment reconciles the same transaction.
5. Close the setup advertisement and clear ephemeral admission secrets.

Cancellation and restart follow actual pre- or post-commit state. Abort removes provisional trust before commit; post-commit removal remains explicit. Prepared exact-pin LAN transactions may reconcile without reopening BLE. New admission sessions still require a physically opened candidate window. Scanning and admission run directly on the main; another member does not relay them.

## Original-controller authority and operation

The first successful Matter setup establishes the original controller only after committed fabric, disarmed fail-safe and checked persistent connected Wi-Fi, or proven retained original authority. Receiving Wi-Fi, entering a setup code or reading a portable document cannot create that role. Reboot and last-fabric removal retain it.

An added device commits its headless member role before adoption acknowledgment. It cannot host UI/home integrations, pair a remote, become an input gateway or promote itself. Ambiguous/corrupt identity or membership stays passive for physical recovery.

Exact-peer TLS and original-controller authority protect subsequent owner delivery. Keep stale boot/link/configuration/input and replay fences, separate from runtime equipment lookup. Explicit removal retains the established drain/reset-journal/result-attempt flow; generic loss or setup abort does not erase a device. [Recovery](13-faults-removal-and-recovery.md).

## Evidence

Test direct discovery and exact candidate selection, physical-window eligibility, both profiles, mismatched-profile rejection, wrong-code/proof/replay failure, encrypted credential transfer, exact-pin LAN proof, storage cuts and role-before-service boot. Physical radio interoperability and production secret storage require separate evidence. Peer reports are not hardware attestation or proof of HDMI response.
