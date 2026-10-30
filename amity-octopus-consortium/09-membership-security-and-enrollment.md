# Identity, membership and adding a device

Membership protects who may deliver work to an owner. It does not approve HDMI routes or replace local Amity algorithms. [Transport](08-consortium-transport-and-routing.md), [shared radio/setup](10-ble-and-matter-coexistence.md), [storage](11-configuration-storage.md) and [original-controller policy](21-original-controller-policy.md) apply.

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

The `fw_reset` task reserves 6 KiB for the existing synchronous setup callback,
which may close Matter commissioning and refresh DNS-SD during radio handoff.
The manual-Matter status/configuration branch is a separate `noinline` helper
so its large local buffers do not occupy the Add callback's frame. Gesture
thresholds, callback/task ownership, priority and radio ordering are unchanged.
Failure logs identify radio reservation/window errors; the post-callback
high-water diagnostic reports remaining stack bytes.

Normalize names to lowercase after trimming surrounding whitespace: 1–32 letters, digits or hyphens, with no leading/trailing hyphen. Bind the selected candidate ID and proposed hostname into admission before applying credentials or membership. A hostname is metadata, never authority or equipment identity. Do not turn old cosmetic labels into hostnames during migration.

## Admission profiles

The normal build uses fresh ephemeral key exchange and AEAD-protected messages during the physically authorized window. Sensitive Wi-Fi and membership records travel only after the encrypted session is established.

`CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE`, default off, selects the stricter build. The user enters the candidate's existing printed Matter manual setup code or QR payload; PAKE proves code possession and authenticates the fresh Amity admission session keys. It does not run full Matter commissioning, add a fabric or create an original-controller role through Matter.

Both main and candidate must use the same profile. Bind the profile and participants into the exchange and reject profile mismatch, malformed records, bad proofs and replay without downgrade or fallback. Keep setup codes and derived secrets transient; do not write them to browser storage, logs or portable configuration.

Startup runs `admission_crypto_self_test(configured_admission_mode())` so a
normal-profile boot does not execute the unused strict-profile PAKE test.
Failure still keeps admission closed. The no-argument self-test used by explicit
USB diagnostics and native coverage continues to exercise both profiles;
profile matching and rejection without downgrade remain unchanged.

## Commit the existing membership transaction

After the selected profile establishes the encrypted session, retain the existing bounded enrollment transaction:

1. Bind candidate/controller IDs, installation, proposed headless role, join identity/digest, membership revision, operational leaves/pins and canonical hostname.
2. Persist prepared records and commit/validate the candidate hostname before network staging and acknowledgment. Seal candidate advertising once prepared, while the same encrypted connection may finish network transfer. Provisional trust grants setup/status only.
3. Commit supported network credentials through the existing CHIP Wi-Fi owner when needed, then prove the prepared exact-pin LAN path with a fresh challenge.
4. Commit membership durably and idempotently on both sides before reporting ready. A lost acknowledgment reconciles the same transaction.
5. Close the setup advertisement and clear ephemeral admission secrets.

Proposal construction uses the exact boot-validated `Identity` and the
candidate `Peer` just verified in the authenticated admission exchange. Copy
their unchanged IDs, DER and derived pins into the canonical proposal instead
of repeating two public-key signature checks. Untrusted candidate identity and
certificate validation still happens before constructing or saving the proposal;
the candidate still compares its own exact identity/DER/pin and validates the
new authority certificate. No credential cache or alternative trust path is
introduced.

The main explicitly completes ATT MTU negotiation on the selected live
connection before discovering its GATT service and exchanging records. A
preferred MTU setting alone is not negotiation. Completion, including an
already-pending exchange, must match the current connection and callback
generation. Retain the five-second GATT-step bound and the 30-second record
assembly deadline; do not extend those limits to mask fragmentation failures.
The negotiated MTU determines fragment payload size within the ATT value limit.

The CHIP Wi-Fi driver's successful connection callback can precede DHCP. Network
staging therefore awaits a usable IPv4 address within the same total 35-second
connection budget, measured before dispatching the driver request. Association
alone cannot acknowledge a usable prepared LAN path. Failure retains the
existing rollback; this adds no second connection timeout or retry.

When a prepared/member device already has the authority's numeric IP, its
first connection attempt uses that address before a blocking mDNS query.
Failure retains the existing discovery, address rotation, backoff and exact
pinned TLS authentication. The required member restart and enrollment handshake
remain unchanged. This avoids an initial discovery delay without treating an
IP address as authority or skipping a failed-route fallback.

A prepared candidate retains the existing durable acknowledgment and 500 ms
handoff delay before rebuilding under its member identity. Its link task uses a
PSRAM stack, so invoke that same restart through the existing internal-flash-stack
broker: shutdown hooks may write NVS after disabling the flash cache. Failure
to admit the broker handoff remains closed; do not restart on the unsafe stack.

Retiring original interfaces removes their administration mDNS service only if
this process registered it. Serialize registration, refresh and removal, and
remove that owned service before stopping Matter's shared responder. A headless
member did not register the administration service and must not remove it from
an absent responder. This is resource lifetime handling, not a new role or radio
policy.

The Add operation's transient status becomes `complete` only after the exact
selected peer is live/ready with the requested canonical hostname. Retain that
completed operation result until a new scan replaces its in-memory view, even
if the member is subsequently removed. Completion reports a finished operation;
current membership and settings still require their own live checks. This adds
no durable activity journal, execution approval or new API.

Cancellation and restart follow actual pre- or post-commit state. Abort removes provisional trust before commit; post-commit removal remains explicit. Prepared exact-pin LAN transactions may reconcile without reopening BLE. New admission sessions still require a physically opened candidate window. Scanning and admission run directly on the main; another member does not relay them.

## Original-controller authority and operation

The first successful Matter setup establishes the original controller only after committed fabric, disarmed fail-safe and checked persistent connected Wi-Fi, or proven retained original authority. Receiving Wi-Fi, entering a setup code or reading a portable document cannot create that role. Reboot and last-fabric removal retain it.

An added device commits its headless member role before adoption acknowledgment. It cannot host UI/home integrations, pair a remote, become an input gateway or promote itself. Ambiguous/corrupt identity or membership stays passive for physical recovery.

Exact-peer TLS and original-controller authority protect subsequent owner delivery. Keep stale boot/link/configuration/input and replay fences, separate from runtime equipment lookup. Explicit removal retains the established drain/reset-journal/result-attempt flow; generic loss or setup abort does not erase a device. [Recovery](13-faults-removal-and-recovery.md).

During explicit removal, replies admitted while the peer was ready may already
be in flight when its authority is fenced. The lifecycle receive loop may
discard heartbeat, execution-lease and settings-result envelopes only after
exact group/peer/destination/boot/epoch/configuration checks and the ordinary
management capability, TTL and replay validation. Restore the lifecycle-only
restriction immediately; do not apply the discarded reply or admit ordinary
work. The existing deadline and 32-frame receive bound still apply while waiting
for the retirement acknowledgment. Wrong-scope, expired, duplicate or unsupported
traffic fails the exchange rather than loosening its authorization.

## Evidence

Test direct discovery and exact candidate selection, physical-window eligibility, both profiles, mismatched-profile rejection, wrong-code/proof/replay failure, encrypted credential transfer, exact-pin LAN proof, storage cuts and role-before-service boot. Physical radio interoperability and production secret storage require separate evidence. Peer reports are not hardware attestation or proof of HDMI response.

The actual-source admission regressions are
[`consortium_admission_startup_test.py`](../tests/consortium_admission_startup_test.py),
[`consortium_admission_crypto_test.py`](../tests/consortium_admission_crypto_test.py)
and [`admission_workflow_test.py`](../tests/admission_workflow_test.py).
They cover selected-profile boot/failure gates, full explicit self-tests, exact
validated proposal records and known-address connection with authenticated
fallback. The complete 0.1.105 physical suite and four clean Add timing samples
are recorded in [validation](VALIDATION.md), with the failed diagnostic baseline
retained separately.
