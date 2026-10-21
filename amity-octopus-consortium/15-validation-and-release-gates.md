# Validation and release gates

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

Status: proposed validation plan v0.1 for baseline `d50af8026938797fc0820d0107f4c4e064c2d2bc`. The architecture has not been implemented or hardware-validated by this document set. Existing test source is evidence of available coverage, not evidence that the proposed changes already pass.

## Purpose and evidence standard

Validation must prove three separate things: local CEC timing/protocol correctness, safe coordination across owners and failures, and the user-visible behavior of the complete HDMI installation. A host simulation cannot prove electrical isolation or HDMI picture continuity. A browser fixture cannot prove firmware execution. A successful command acknowledgement cannot prove equipment state.

Each result records source/build commit, SDK/dependency identity, hardware profile, firmware/security profile, configuration revision/digest, participating OctopusIds/BootIds, topology, test seed, faults injected, observations and pass/fail outcome. Protect credentials and raw sensitive diagnostics; publish a redacted summary and retain the reproducible private evidence under controlled access. The repository already separates generated test evidence from source and warns that the output directory can contain secrets. [Evidence handling][b-evidence]

Use deterministic host tests for exhaustive state transitions, bounded integration tests for real services, and hardware tests for timing/wire/video claims. Do not expand testing by repetition without a stated risk: each suite below owns specific invariants and a release decision.

## Baseline suites to preserve

| Existing suite | Coverage to retain | Limit |
|---|---|---|
| `tests/run_host_tests.sh` | Sanitized configuration, local hub, remote policy, version and related core tests | Not the full provisioning/browser/hardware suite. |
| `tests/cec_hub_test.sh` | Core hub, queue/commit behavior, front PA, settling | Assumes the existing two-port architecture. |
| `tests/provisioning_test.sh` | Go tests/vet, factory records, interrupted reset | Must be extended for origin and membership storage. |
| `tests/remote_input_test.cpp` | Stale input, cancellation, held-state quarantine | No network transport or remote-owner migration. |
| `tests/web_*_ui.py` | Browser behavior against local API fixtures | Fixture replies do not exercise device firmware. |
| `tests/cec_front_physical_hardware.py` | Initial back-side PA inference, reboot and rescan behavior | Isolated Pi/DUT fixture with two CEC adapters. |
| `tests/cec_web_activity.py` | API-to-wire activities, route repair, forced standby | Tests reaction to theft, not blocking before a switch receives it. |
| Matter/HomeKit hardware suites | External controller-to-firmware-to-CEC paths | Must gain primary-zone/stable-slot and group failure cases. |

The existing host runners use AddressSanitizer and UndefinedBehaviorSanitizer. Keep that coverage and the exact compiler/tool versions in CI. Provisioning tests run separately from `run_host_tests.sh`; a green result from one is not a green result from both. [Host runner][b-host] [CEC runner][b-cec-runner] [Provisioning runner][b-provision-runner]

## Refactor the deterministic fixture first

`tests/cec_hub_fixture.hpp` runs real admission/worker code, but has a singleton `latest_task`, one global synthetic clock, and GPIO-based assumptions about two peers. Creating five `HubService` instances against this fixture would not prove an octopus works. [Fixture][b-fixture]

Introduce a reusable fixture with these explicit entities:

- An octopus instance with persistent store, boot identity, independent monotonic clock, scheduled tasks, bounded queues, and network endpoint.
- Tentacle ports attached to named electrical CEC domains; several ports can share a domain.
- CEC peers with separate reported OSD names, editable user labels, native/proxy provenance, address-allocation behavior, configurable response delays, malformed frames and unsolicited traffic. A peer can retain its scoped name while changing LA; another peer can immediately reuse the old LA.
- Transport links with controlled delay, loss, duplication, reordering, disconnect and reconnect.
- A scheduler that can pause at queue reservation, persistence commit, configuration-pointer swap, wire completion, owner-local repeat admission and exact hold release/cancellation.

Capture every received/emitted frame with port/domain identity and local observation order. The test oracle must assert both required output and forbidden output. Keep electrical bit timing tests separate from semantic policy tests so a fake bus does not accidentally “prove” correct ACK behavior. A multi-node test must use independently advancing clocks to detect assumptions that remote monotonic timestamps share an epoch.

Use named seedable fault schedules. Store minimal replay traces for failures, but never include setup secrets, session tokens, TLS keys or Wi-Fi credentials. Run the single-tentacle legacy suite through the new adapter as a regression profile before adding consortium behavior.

## Core invariant tests

| ID | Scenario | Required assertion |
|---|---|---|
| V-ADDR-01 | Same logical/physical address and OSD name on different domains/octopuses | Qualified name selectors and runtime observations remain distinct; no global-name merge or wrong-owner command. |
| V-ADDR-02 | One AVR observed on two attachments | Merge only after declared evidence/confirmation; shared resource is controlled once. |
| V-NAME-01 | Unique native OSD selector changes from LA 4 to LA 8 in the same scope | Runtime resolution updates automatically; selectors, activities, slots and both configuration revisions remain unchanged; fresh control reaches LA 8 without edits, confirmation or a new Select/Resume solely for the LA change. |
| V-NAME-02 | Another native device reuses LA 4 while old work/held input remains queued | Old binding work is fenced before wire start; no stale press or cleanup release reaches the new occupant; no queued relative input or hold is moved to LA 8. |
| V-NAME-03 | Equal OSD names on different octopuses, tentacles or sides | Persist separate full scopes and resolve each independently; the resolver cannot pick the first global name or delete the other device's record. |
| V-NAME-04 | Two native devices report the same OSD name inside one scope/shared domain without a supported distinguishing constraint | Resolution is ambiguous and affected control is unavailable; current LA, monitor choice and UI label cannot be persisted as a disambiguation shortcut; any separately supported matching constraint must survive LA churn. |
| V-NAME-05 | A proxy advertises a native device's name; one native frame is seen by several domain monitors | Proxy/native provenance prevents substitution; native binding `device_name` matches a configured `qualified_names` entry; proxy binding has no native `device_name`; duplicate observations do not manufacture another appliance or unverified alias. |
| V-NAME-06 | UI label edit versus matching OSD-name/scope edit; exact name-codec vectors | Label edit is presentation-only; selector edit is semantic and fences old work; permitted OSD values round-trip exactly without trimming/case folding/label substitution; unsupported values do not become LA fallbacks. |
| V-NAME-07 | Legacy migration and export/restore with LA churn, repeated names and missing OSD evidence | Unique legacy names persist as qualified selectors and resolve at the new LA; ambiguous names and synthetic defaults remain explicit; no saved LA gains selection authority. |
| V-DOM-01 | Recabling joins two formerly separate CEC domains | Conflicting virtual-TV/proxy roles quarantine before additional conflicting claims/actuation. |
| V-DOM-02 | More proxies requested than usable logical identities | Configuration fails explicitly; no address reuse or foreign ACK shortcut. |
| V-CFG-01 | Pure display-label rename/reorder during an active activity and held input | Only presentation revision advances; OSD matching selectors remain byte-for-byte unchanged; zero edit-caused CEC traffic; policy/leases/holds, identities and external selectors unchanged. |
| V-CFG-02 | Power loss at every config write/activation operation | Recover old or committed next revision, never mixed state. |
| V-CFG-03 | Semantic edit with changed, unchanged live and unchanged offline owners | Exact-policy rebase is required before new work; stale queued operations fail; unchanged held state retains its exact target under current general authority, and a failure-stopped state cannot restart on rebase/renewal. Offline owners synchronize before actuation. |
| V-EXEC-01 | Stale config/binding/domain/session/route generation | Work is rejected without wire output. |
| V-EXEC-02 | Duplicate command before/after acknowledgement and reboot | Retained duplicates return known outcome; uncertain non-idempotent action is not blindly replayed. |
| V-INPUT-01 | Initial local controller input delayed beyond its original deadline/execution lease, including rollover | Coordinator rejects stale input before planning; actuator rejects stale first delivery before wire start; neither hop grants a fresh lifetime to the old physical event. An already-started intentional hold survives that initial deadline only under current plan/route/link authority. |
| V-INPUT-02 | Several-second successful hold, equivalent local/remote owner, then release; repeated identical remote reports | Exactly one inter-device down and one matching up; executor-local completion pacing matches baseline, with no per-button renewal or timer cadence. Commands and keyed up submit independently while earlier ACKs remain pending; admitted presses finish, finite taps stay finite and each key-press frame has one wire attempt. |
| V-INPUT-03 | Release/cancellation during in-flight or queued press, busy wire, stale up, exact link loss and silent authority expiry | No queued repeat burst or press after cancellation; started frame finishes before exact cleanup. Old tokens/links cannot release newer state; reconnect never replays a hold. Record actual release-attempt timing separately from successful wire delivery. |
| V-INPUT-04 | Initial/later key TX failure, overlapping keys/taps, unchanged down, renewal, then up and fresh down | Failure ends that key's pressed state and repetition; later matching up is a no-op. Final UCR is attempted only when all recipient keys cease being pressed and admitted presses drain, including last-key failure. Same-down/renewal cannot restart; fresh up/down may start again. Cover NACK/arbitration/error/cancel/expiry, pre-start cancellation and failed admission distinctly. |
| V-HOLD-01 | Full CEC FIFO, lost release, disconnected peer, new owner | General authority/liveness fences stop old repetition; stale release cannot cancel a newer hold. Final key-up queues release immediately behind admitted presses; if eight messages are admitted, the release is dropped and logged without executor admission retry. |
| V-QUEUE-01 | Driver FIFO capacity/batch busy, cancellation and supplied attempt budgets | Eight total queued-plus-active messages per wire in all profiles; at eight, releases drop without executor admission retry and every TX dropped because the FIFO is full is logged; no executor outgoing queue or hidden remainder. Explicitly submitted Bus batches admit entirely or enqueue nothing. Finite key recipes submit press then immediate conditional release: at seven the press enters slot eight and release drops; at eight the press drops and no release is attempted. CEC sends supplied frames/budgets/guards without opcode/button policy; executor chooses all presses/repeats/releases. Completion metadata survives through terminal drain. |
| V-SCAN-01 | Concurrent scans, noisy bus and key release | Single scanner per domain, bounded jobs/snapshots, release watchdog remains live. |
| V-PART-01 | Coordinator unavailable, asymmetric partition, stale reconnect | No election or unauthorized singleton transition; local isolation remains effective. |
| V-BOOT-01 | Coordinator restart, member restart and same-boot reconnect | Restarted desired sessions are suspended/degraded without automatic wake/route/standby; fresh intent creates new runtime IDs; verified same-boot recovery never replays relative input. |
| V-RETIRE-01 | Explicitly remove responding/unresponsive committed member; generic retirement; reboot or update retired node | Revoked pins stay rejected. Responding removal drains the native owner, commits the existing reset marker, attempts the exact result and restarts only the target; lost reply never repeats reset. Unresponsive removal requires physical-button reset. Generic retirement, authority loss, enrollment abort and main/self targets never reset. |

Fault coverage must include a crash after local CEC transmission but before outcome reporting. The expected result is an unknown equipment outcome requiring observation/reconciliation, not an exactly-once claim. [Transport](08-consortium-transport-and-routing.md) [Execution](07-activity-planning-and-execution.md)

For name-resolution cases, retain before/after portable configuration and runtime snapshots as separate evidence. Compare `qualified_names`, ActivityIds, external slots and both configuration revisions, then assert the intended runtime binding revision/address changes. Capture wire traffic on both the old and new addresses, and count UI configuration/confirmation requests so automatic resolution is actually demonstrated. A test that edits the activity or injects a replacement confirmation before control succeeds does not pass V-NAME-01. Exercise missing-name/late-name discovery and a same-name conflict arriving after an initially unique resolution. Configured aliases across different native attachments require explicit verification; equality of OSD text alone must not create them.

## Proving source-theft prevention

The central product requirement is that an unwanted source cannot cause a downstream switch/display to change route by sending CEC through its isolated boundary. Repairing the route after the receiver changes it is insufficient.

Build a fixture with selected and inactive sources, an AVR/switch behavior model or real receiver, and a display. Place an inactive source behind its own tentacle. Capture the source-facing isolated CEC segment, the corresponding AVR-facing segment, and any downstream control segment capable of changing the route. Keep selected video on a known test pattern.

For every forbidden opcode/payload class in the [message policy](06-cec-roles-and-message-policy.md), inject valid and malformed forms, repeats and bursts. Include Active Source, route/view-on attempts, standby/power interference, and vendor-specific behavior enabled by the profile. Exercise each condition during idle playback, an authorized activity transition, configuration staging/application, discovery, coordinator loss, reconnection and local reboot.

Pass requires:

1. The originating forbidden message is present on the isolated source capture, proving the stimulus occurred.
2. No equivalent unauthorized route/power action is re-emitted on any protected AVR/display domain, including indirect semantic translations.
3. The selected route and video output do not change because of that CEC stimulus. Record receiver/display state and instrument the video path or optical output sufficiently to detect a brief interruption, not merely its final state.
4. Permitted selected-source control and local key release still operate under the same load.

Use at least 100 repetitions per supported hardware topology and threat class for the hardware regression, plus a deterministic seeded stress run of at least 1,000 policy injections in the host fixture. These are initial test-plan counts, not a statistical proof against all equipment behavior. Retain every failure, including brief interruptions repaired automatically.

HDMI hot-plug, EDID changes, video timing renegotiation or an AVR's non-CEC automatic switching are separate mechanisms. Test and report them separately; do not attribute a non-CEC picture interruption to a successful CEC filter, and do not claim this feature prevents every possible source-selection mechanism. Required product behavior and its supported equipment profiles must be explicit.

The baseline hardware activity test specifically checks forced back-side standby after source theft. Keep it as a regression for legacy behavior where applicable, but replace it as the release criterion for preventative isolation. [Existing theft check][b-theft]

## Topology coverage

| Profile | Required arrangement and result |
|---|---|
| T-01 | One octopus/one tentacle in the original TV–AVR arrangement; preserve supported legacy behavior and migration. |
| T-02 | One octopus with each supported tentacle count 1 through 5; every enabled front/back line independent and correctly addressed. |
| T-03 | TV and projector, source attachments on direct display inputs and AVR inputs, with explicitly modeled AVR outputs/capabilities; select each feasible route and reject infeasible ones. |
| T-04 | Two tentacles whose sink-facing ports share a CEC domain; one domain authority, legal distinct identities, no message amplification. |
| T-05 | Two octopuses in separate rooms on LAN with shared control resources where physically connected; reject video routes that no HDMI path supports. |
| T-06 | Initial consortium target of eight octopuses under the maximum supported local port profile; bounded memory/sockets and stable behavior under partial failures. |
| T-07 | Repeated OSD names, LA churn/reuse, duplicate physical addresses in separate domains, sleeping sources and late identity reports; persistent names stay stable, unambiguous names resolve automatically, and conflicting native matches stay unavailable. |
| T-08 | Recabling, source replacement, AVR output differences and domain merges; changes to a configured selector's scope require explicit semantic configuration, while ordinary same-scope LA changes follow V-NAME-01 and do not trigger a replacement workflow. |

T-03 must not assume all receivers expose CEC on only one output. Record actual capabilities of each tested receiver and verify how its outputs share control, sources and audio resources. T-06 is a release validation target; it is not an unbounded protocol claim. If fewer nodes/ports are tested, advertise only the tested cap and record the larger profile as blocked.

## Timing, memory and coexistence gates

Measure the proposed shared timer at the advertised engine capacity while Wi-Fi carries peer control, BLE maintains its remote link, enrollment is attempted, web clients poll, HomeKit/Matter traffic runs, logs are downloaded and an OTA image is written. Qualifying a five-tentacle profile requires ten engines; a reduced supported profile must state its lower measured capacity. Include flash erase periods, not only quiet steady-state traffic.

The runtime owner must publish the approved sample period, maximum ISR execution time, worst sampling gap/jitter, task priorities, queue bounds and minimum internal-memory margin from [runtime](04-hardware-and-cec-runtime.md). The release gate cannot pass with these thresholds unset. Measurements must distinguish average, percentile and worst observed values; an average below the budget does not excuse a protocol deadline miss.

Compare local and distributed traces directly with current `main` (`98e64a6` for this merge): each successful completion permits only that key's continuation; submit another key or CEC command immediately while ACKs remain pending. Normal keyed up must leave other holds and admitted frames unaffected. Apply the explicit user exceptions: every key-press frame has one attempt; any failed key TX ends that key's pressed state and repetition and makes later matching button-up a no-op. The executor derives global UCR from the end of the receiving device's stream, ordered behind admitted presses without waiting for their completion, once all keys there cease being pressed, including last-key failure. Cover overlapping keys, finite taps during another hold, independent recipients, and pending final release before a new press. No unchanged down, policy refresh or general authority renewal may restart it. Test holds lasting several seconds beyond initial delivery freshness, with exactly one inter-device down/up and no per-button traffic. Cover equivalent local/remote audio targets, finite taps/Siri swipes, first/later failures, independent command/up admission while another key awaits ACK, busy/arbitrating wire, cancellation and stale tokens. Verify the executor has no outgoing frame queue, direct full admission returns busy, generic explicitly submitted Bus batches are atomic into CEC's sole FIFO with eight total messages while finite key recipes preserve sequential direct admission, and the CEC code contains no opcode-based button/repeat/release policy. Test rejection of ordinary and release frames at eight admitted messages, logging of every TX dropped because the FIFO is full, and no later retry of a dropped release. Check isolated finite taps at six/seven/eight occupied slots: at six both press and release may enter; at seven the press enters slot eight then release is dropped and logged; at eight the press is dropped and logged with no release attempt. Separately test generic explicit batch atomic rejection and per-frame logging, and completion/guard capacity through all eight admitted messages per wire. On link/plan-authority loss, verify immediate suppression and exact context fencing; general authority liveness supplies the silent-stall bound without a per-button timer. Distinguish a release decision/admission from actual successful wire delivery.

Algorithm parity is a release requirement across existing functionality, not just held keys. Compare relevant baseline source and tests for each changed path, including activity/AVR/audio/switch behavior, discovery, physical-address selection, remote semantics, CEC retries and approved UI workflows. List explicit user-requested changes and necessary distributed fencing adaptations separately from preserved behavior. A newer proposal or test that encodes changed behavior is not evidence of authorization.

Siri remote swipes are explicitly included. Replay the same input traces against the original and current gesture/sequence implementations and compare recognition thresholds, direction/acceleration, timing, press/release order, finite-step completion and cancellation on disconnect/activity changes. Repeat with local and remote equipment owners. Host trace parity does not establish physical touch/RF/CEC behavior; record that qualification separately.

Track free internal heap, largest internal allocation block, PSRAM use, stack high-water marks, socket use, queue saturation, dropped RX and TX errors. Verify no unbounded growth over an eight-hour mixed-load soak; retain minute samples and fault events. The baseline already directs TLS allocations to PSRAM but has a 16-socket ceiling, three BLE connections and four BLE bonds. Group expansion must be measured against those limits. [Resource defaults][b-limits]

## Security, storage and update tests

Test enrollment with wrong/expired/replayed codes, hostile discovery advertisements, a changed leaf key, wrong consortium/OctopusId, removed member, swapped destination, and authorization to the wrong role. A valid TLS channel with an unauthorized role must still fail. Cold boot without trusted wall time must preserve exact enrolled-key pin validation and private-key proof; it must not switch to globally unverified TLS.

For origin upgrade, inject power loss at each record write, final marker and one-time-code delivery boundary. Verify stable OctopusId, unchanged factory-v3 bytes, explicit lost-code rotation, and no code in logs/export/browser history. Test buttonless USB authorization separately. Concurrent Matter/consortium/remote pairing operations must serialize through one radio arbiter without tearing down the NimBLE host. [Enrollment](09-membership-security-and-enrollment.md) [Coexistence](10-ble-and-matter-coexistence.md)

Extend the existing reset interruption fixture to every new namespace. After reset, origin survives, operational membership/keys and application configuration do not, and reset completion is idempotent. Also prove a normal boot without a reset marker leaves persistent state untouched. [Reset test][b-reset-test]

OTA tests include invalid publisher signature, correct manifest/wrong image, wrong target/profile, oversized image, unsupported protocol/schema, interrupted transfer, power loss during selection, failed boot health, coordinator outage during an executor update, retired-node update, and rollback after a storage feature barrier. Record production secure-boot/encryption evidence on explicitly provisioned test hardware. Unsigned development updates cannot satisfy the production gate. [OTA contract](14-ota-and-compatibility.md)

## UI and integration acceptance

[Baseline user interface](20-user-interface.md) owns the normative UX acceptance cases **UX-01 through UX-16**, normal vocabulary, naming presentation and question order. The UI implementation's evidence record must map each applicable case to an observed result; the requirements below supplement those cases with firmware/integration evidence. Their inclusion here does not mean they have been implemented or tested.

Run browser tests at desktop and narrow-phone sizes against fixture APIs and real firmware. Verify visible progress, no duplicate action on double click, stale-editor conflicts, unavailable owners, pending membership, removal/replacement, unknown equipment state and secret clearing. A single login must manage a healthy consortium without opening satellite administration sessions.

Observe users completing the unchanged baseline Activities/Remote/Matter/HomeKit/Advanced workflows and Amity-device management on desktop and narrow-phone browsers. Compare fields, controls and keyboard/focus/error behavior to the pinned baseline and its approved amendments. Verify the exact tab order Activities, Remote, Matter, HomeKit, Amity devices, Advanced, with Advanced last. Record assistance and accessibility results; do not substitute historical wizard fixtures for this evidence.

Verify baseline device-list choices carry full scope behind canonical Amity-hostname context. Rename/reorder preserve stable IDs, external slots and aliases; stale editors reject without silently replacing current configuration. Discovery, device-list refresh and Save emit no wake/routing/standby. Explicit Start uses current configuration, original input deadline and ordinary planner/resource guards.

Change an appliance LA while retaining its unique scoped OSD name, then use web/remote/integration activity controls without editing the activity. Identical names on separate attachments remain separately selectable; same-scope ambiguity and missing devices remain unavailable. Proxy identities and friendly labels never overwrite native selectors. Supplement deterministic fixtures with a real-device LA-reallocation case before qualifying that hardware behavior.

With actual Matter and HomeKit controllers, migrate an existing paired installation and prove its primary-zone controls, 16 input mappings, aliases and hidden settings survive display-label rename, reorder, LA churn and reboot. Deleted slots must not select another activity. Exercise multiple zones through the original controller's browser and sole remote while integrations retain their existing projection. Members must expose no HTTP/admin/recovery UI or operational home-control service. Reject live controller replacement/promotion. Separately test manual failed-main recovery on fresh Matter-enrolled hardware: existing Advanced restore retains logical IDs/names/order, requires old-main isolation, imports no secrets/old roster trust and emits no positive CEC; remote/home integrations are paired again and headless members are reclaimed/re-enrolled. No new wizard or primary-zone chooser is added. [Control surfaces](12-control-surfaces-and-admin.md)

## Release gates and owners

| Gate | Owner | Evidence and decision |
|---|---|---|
| G-01 Contracts and schema | Architecture owner | Canonical persistent name selectors versus runtime addresses, IDs/revisions, bounded schemas, capability/operation vectors and cross-document consistency approved. |
| G-02 Core and simulator | Runtime/test owners | Legacy regression plus all V-series invariants pass under sanitizers and deterministic fault injection. |
| G-03 Electrical runtime | Hardware/CEC owners | Board profile, timing at advertised engine capacity, foreign-address ACK behavior, isolation while off and recabling behavior measured. |
| G-04 Prevention and topology | HDMI compatibility owner | Required T-series profiles plus forbidden-egress and no-CEC-induced-interruption evidence. |
| G-05 Trust and lifecycle | Security owner | Enrollment, role/pin enforcement, revocation, origin/reset and cold-boot policy tests pass. |
| G-06 Storage and migration | Storage owner | Worst-case occupancy and every interrupted migration/reset/update boundary recover correctly. |
| G-07 Control surfaces | UI/integration owner | Real-controller migration and primary-zone/slot behavior verified; applicable UX-01 through UX-16, observed novice completion/question counts and narrow-screen journeys have evidence. |
| G-08 Fleet/coexistence | Firmware/release owners | Mixed-version update matrix, eight-hour load results and tested size cap established. |
| G-09 Production security | Product/security owners | Signed release, verified boot, encrypted secrets and physical recovery process demonstrated. |

A development release may explicitly omit G-09 and must be labeled accordingly. It cannot waive isolation, authority, identity or storage-recovery correctness. Unsupported hardware roles, larger consortium sizes, extra external zones and ARC/eARC remain outside the qualified profile. Election, promotion and live controller transfer are not implemented or planned.

The final release record lists passed, failed, not-run and inapplicable checks separately. Tests whose fixtures were only reviewed remain not run. The coding agent completing a workstream links its evidence and unresolved gates in the [agent breakdown](16-agent-work-breakdown.md); it must not mark the architecture complete merely because firmware builds.

[b-evidence]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/test-results/README.md
[b-host]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/run_host_tests.sh
[b-cec-runner]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_hub_test.sh
[b-provision-runner]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/provisioning_test.sh
[b-fixture]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_hub_fixture.hpp#L24-L139
[b-theft]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_web_activity.py#L184-L198
[b-limits]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/sdkconfig.defaults#L173-L225
[b-reset-test]: https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/factory_reset_test.cpp#L53-L99

## Superseding original-controller and baseline UI matrix

Run SC-01–SC-08 from [document 21](21-original-controller-policy.md), including unknown/unbound identity migration, foreign/second remote rejection, adoption while paired/connected, queued management cancellation, no member UI/listeners/integration endpoints after reboot, legacy promotion replay and controller loss. Compare the rendered UI and interactions to commit d50af8026938797fc0820d0107f4c4e064c2d2bc: original forms and approved amendments, including canonical Amity-hostname context and the exact tab order Activities, Remote, Matter, HomeKit, Amity devices, Advanced (Advanced last). AVR/audio/switch workflows remain. No negative authorization test may require deleting a real user bond.

## Per-device adapter preference acceptance

Run the [document 20 settings matrix](20-user-interface.md#per-device-hdmi-adapter-settings-amendment--20-october-2026): zero-to-five and sparse stable slots, legacy fixed presence, unknown/no detector, explicit removal/reinsert ABA, independent defaults/detected/selected values, stale boot/settings/generation/deadline, busy held activity, failed/late storage and lost applied response. Opening or refreshing Settings must generate no CEC traffic. Only exact durable application may restart the selected device, after response transmission attempt; an unknown client outcome never auto-replays. Preserve original activity forms and all Advanced workflows except the expressly moved global PA form.

Source/host evidence includes [actual native owner tests](../tests/native_ports_test.cpp), [typed runtime tests](../tests/live_system_test.cpp), [atomic settings/service tests](../tests/adapter_settings_test.cpp), [browser settings tests](../tests/web_device_settings_ui.py) and the deployed strict peer settings codec. Host/browser passes establish software boundaries only. Physical sparse connector/detector electrical qualification, real remote owner restart/lost-ACK recovery, and post-restart independent address behavior require separate recorded DUT/member evidence.
