# Identity, membership and adding a device

Membership protects who may deliver work to an owner. It does not approve HDMI routes or replace local Amity algorithms. [Transport](08-consortium-transport-and-routing.md), [shared radio/setup](10-ble-and-matter-coexistence.md), [storage](11-configuration-storage.md) and [original-controller policy](21-original-controller-policy.md) apply.

**1 November 2026 membership limit.** One original main supports at most three
secondary Amities, for four devices total. A saved secondary occupies its place
while offline until explicitly removed or replaced. Runtime link slots, admission
scan/submission/preparation and credential roster reads/writes use the same
three-secondary bound; adding a fourth is rejected. The stored group record
allows 8 KiB so three maximum-size accepted certificate leaves fit. Existing
authentication, physical-window, authority, revision and transaction fences apply.
Host regressions exercise a third admission, rejection of a fourth, three-peer
startup/persistence and exact-leaf TLS authentication. These checks do not
qualify physical four-device operation.

## Identity and credentials

Stable random Amity device IDs identify hardware independently of MAC, hostname, IP address and Matter identity. Installation identity, durable original-controller/member roles, operational private keys and exact peer leaf pins retain their separate purposes. Private keys stay on their device. Origin IDs and factory Matter/admin credentials survive the ordinary factory-reset contract; operational group trust/network/roles are reset-owned.

There is no separate Add secret, stored bootstrap verifier or USB Add-code provisioning/rotation step. Startup discards the obsolete legacy Add-verifier record without loading it; an erasure failure is a warning, never an admission gate, and does not change origin or factory credentials. Adding a device does not require restoring old provisioning artifacts. The factory Matter code remains available for ordinary Matter setup and the optional stricter Amity admission profile.

## Physical workflow

At boot, automatically open the bounded Matter setup window when the current fabric count is zero and the board is either unclaimed or the proven original main with retained authority. This includes factory-reset and still-unclaimed reboots and requires no FW RESET GPIO. It is a permanent user requirement, with no commissioning-history latch or saved-activity/configuration gate. A main whose last fabric was removed keeps its main role/UI and reopens setup at boot. Boards with a fabric, and headless/prepared/retired/recovery roles without one, do not open it automatically. The original main also retains explicit Matter pairing controls.

On the original main, open **Amity devices → Add** to scan directly for nearby unclaimed boards. Hold the candidate's **FW RESET** for at least three seconds and release before ten seconds. Choose the candidate if several are found, enter a valid unique **Name**, then add it. The candidate must remain within the existing bounded physical setup window. A ten-second hold retains its factory-reset meaning; the shorter setup action must not fire first.

The main publishes candidates as soon as discovery finds them. The same bounded
scan continues collecting other candidates while the dialog remains open; the
user can select an available board immediately. Selecting a candidate fences
and cancels that scan before connecting to the exact selected token/address.
Retain the scan generation, cancellation, radio ownership and physical-window
checks. The 20-second collection duration is not a minimum wait before Add.

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

Admission self-tests run only during an explicit user-requested Add attempt.
The main runs `admission_crypto_self_test(configured_admission_mode())` after
the user selects a candidate and submits Add, before connecting or sending
credentials. The candidate runs the same selected-profile test on its existing
GATT worker when the first complete handshake request arrives within its
physically opened setup window. Its transient result belongs to that window;
another connection in the same window reuses it, while a new physical window
tests again. Mere boot, discovery, an idle physical window and ordinary Matter
commissioning do not run the admission test. Operational startup still
initializes PSA and fully validates stored group credentials and its own
identity/key pair before authenticated TLS. An admission diagnostic failure
cannot prevent the independent Matter path from establishing the original main.
The no-argument explicit USB self-test and native coverage continue to exercise
both profiles; profile matching and rejection without downgrade remain unchanged.

A failed candidate remains discoverable within that same bounded physical
window and rejects the handshake with a fixed diagnostic. The existing BLE
record response uses status 2 with header byte 9 set to 1 for a setup security
check failure; the diagnostic has no body and is valid only on the plaintext
handshake endpoint. Other responses retain a zero diagnostic byte. The primary
accepts the reason only on the exact current connection, record and endpoint;
unknown or malformed reasons remain generic failures. This unauthenticated
diagnostic is display-only and never supplies identity, authority, membership
or permission to exchange credentials. The Add dialog displays:
"The new Amity reported that its setup security check failed. Restart it and
try again." A failed main-side test displays: "This Amity failed its setup
security check. Restart it and try again." No encrypted setup handler or
membership/credential transfer runs after a failed check. Long-running test
completion rechecks physical-window, connection/session and cancellation fences
before permitting handshake work or publishing a diagnostic.

A public startup read always loads and validates fresh storage. Within
`runtime_start`, a private shared validator consumes its already validated group
and returns the same validated identity instead of loading and verifying both
again. Keep all authority, local-ID, role, obsolete-record and journal checks;
an unclaimed read-only preflight still does not create an identity. This is
transient reuse inside one startup call, not a cross-call credential cache.

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

The BLE response cursor is written once for each new requested fragment offset.
Pending reads at that same offset do not rewrite it; the server publishes each
complete reply atomically without changing the cursor. Keep the original ATT
and reply deadlines, pending-read delay, record/framing/authentication checks and
exact fragment bytes. This removes redundant writes, without claiming a smaller
total radio-operation count while a long candidate operation is pending.

The CHIP Wi-Fi driver's successful connection callback can precede DHCP. Network
staging therefore awaits a usable IPv4 address within the same total 35-second
connection budget, measured before dispatching the driver request. Association
alone cannot acknowledge a usable prepared LAN path. Failure retains the
existing rollback; this adds no second connection timeout or retry.

When a prepared/member device already has the authority's numeric IP, its
first connection attempt uses that address before a blocking mDNS query.
The member first waits for its own station interface to be up with nonzero
IPv4. Those existing worker turns do not consume the known hint, perform mDNS
or advance retry backoff; they still advance lifecycle and radio cleanup.
After a real connection failure, retain the existing discovery, address rotation,
backoff and exact pinned TLS authentication. The required member restart and enrollment handshake
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

## Operational TLS validation

Full certificate signature validation remains at every untrusted ingress:
identity/group storage load, candidate identity admission and candidate-side
preparation of the authority record. Operational TLS receives only those exact
validated peer records. Setup still parses and checks the required leaf profile,
qualified DNS identity, client/server usages and derived SPKI pin. Its callback
matches the presented leaf byte-for-byte to the captured allowlist and expected
peer identity; it does not repeat the expensive full self-signature verification.
The SDK still requires mutual TLS, certificate verification, the configured
hostname/profile and private-key possession. Preserve chain/depth rejection,
non-clock verification flags and current runtime DER/pin/authority fences.
This introduces no alternative trust source or admission fallback.

Terminal TLS receive diagnostics report a static failure category and numeric
SDK/progress metadata in the existing link-closure log. They contain no frame
contents and do not change assembly, inactivity or whole-operation deadlines.
The outer operational five-second guard retains its existing static timeout
reason and may fire before a numeric TLS receive record is captured.

## Setup timing diagnostics

Ordinary `setup_timing` logs trace admission submission/worker dispatch, BLE
connection and record exchanges, prepared LAN authentication, durable enrollment,
member restart/reconnection and initial configuration synchronization. Stage
records use monotonic device milliseconds with static stage/result labels,
peer/link/scan identifiers and bounded scalar counters. BLE summaries report
negotiated connection parameters and ATT operation/wait counts; TLS summaries
separate setup validation, SDK-call wall intervals and explicit retry waits.
They contain no setup codes, keys, credentials, certificates or record contents.
Timing emission occurs after transport/runtime guards release.

These are elapsed wall intervals, including scheduling and concurrent work,
not CPU measurements. Nested stages overlap and must not be added together;
summed callback/validation intervals are explicitly distinguished from contiguous
spans. The original authentication, record bytes, polling, deadlines, restart,
trust validation and enrollment decisions remain unchanged. Read the existing
authenticated UI log endpoint; USB monitor attachment can reset a candidate and
is not an observationally neutral enrollment measurement.

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
They cover admission-independent boot and Matter claim, selected-profile Add
failure gates, full explicit self-tests, exact validated proposal records and
known-address connection with authenticated fallback. The actual GATT worker and
record-framing regressions cover deferred checks, window/session fencing and
bounded candidate diagnostics. The complete 0.1.105 physical suite and four clean Add timing samples
are recorded in [validation](VALIDATION.md), with the failed diagnostic baseline
retained separately.
