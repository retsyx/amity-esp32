# CEC roles and message policy

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

**Status: proposed implementation specification v0.1.** This document defines what an Octopus may place on each local CEC wire. It is the authority boundary between the activity planner, authenticated consortium transport, local runtime, and unauthenticated HDMI equipment. The implementation baseline is Amity-esp32 commit `d50af8026938797fc0820d0107f4c4e064c2d2bc`.

## 1. The source-isolation guarantee

To prevent a source from stealing an AVR or display input through CEC, place a tentacle **between that source and its first shared CEC junction**. Every source that needs this guarantee must be isolated, including sources connected directly to a TV or projector. A tentacle only between the AVR output and TV cannot stop a source from switching the AVR before the message reaches that output.

The local rule is preventive: an unwanted source's activation, routing, standby, or vendor-control message is never re-emitted into the AVR/display domain. The source can transmit on its own isolated back domain; that transmission does not cross the physical CEC cut. Software may record it and answer valid local identity queries, but cannot translate it into authority to change the system route.

This guarantee covers **CEC-induced changes propagated by Amity**. HDMI HPD, EDID, HDCP, video-mode renegotiation, native vendor signal-detection switching, and actions from an unisolated device remain separate mechanisms. CEC filtering cannot promise absence of every visible blank or change. The hardware passes those non-CEC paths through; implementing a DDC/HDCP proxy or video switch is outside this feature. See [hardware scope](04-hardware-and-cec-runtime.md).

The gate applies before membership activation, during reboot, while offline, and after retirement. No default state bridges CEC. Losing the network does not trigger a local “repair input” routine or broadcast standby. The existing [Hub::source_thief()](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp) reacts by sending standby and restoring an input. That behavior must be replaced for consortium operation; four independent copies would compete over shared equipment and still act after a source's message had reached the AVR.

## 2. Four explicit port roles

Physical orientation and protocol role are separate. A back port connected to an AVR output is not automatically entitled to become a TV; another tentacle may already own the virtual TV identity on that same electrical domain.

| Role | Identity and ACK ownership | Permitted behavior |
| --- | --- | --- |
| `disabled` | None; output released. | No control or discovery transmission. Optional diagnostics must not drive the line. |
| `monitor` | None; observe all valid frames without ACKing foreign addresses. | Observations and explicitly scheduled bounded discovery probes; no equipment control. |
| `tv_controller` | The domain's designated virtual TV, ordinarily address 0 and PA `0.0.0.0`. | Local identity replies and authorized source/AVR control operations on that domain. |
| `playback_proxy` | One assigned playback identity with a verified attachment-specific PA. | Local identity replies, authorized display control, and active-source announcements only under the current route grant. |

A domain has one policy authority and at most one designated virtual TV. Several playback proxies may legitimately share the domain, each with its own address and attachment. Poll and allocate each identity once. A real TV already occupying address 0 blocks an incompatible virtual-TV assignment; do not conceal the conflict by automatically claiming address 14. [Discovery](05-discovery-and-topology.md) owns validation and quarantine.

Normal playback candidates are 4, 8, and 11. Addresses 12 and 13 are conditional CEC 2.0 backup candidates and require a qualified compatibility policy. Four tentacles terminating on one TV or receiver may therefore exceed usable identities. Reject an unsupported allocation with an actionable capacity explanation; do not reuse an address or impersonate a recorder/tuner to fit. The baseline [claim_addresses()](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub_service.cpp) is a migration seam, not a sufficient multi-domain allocator.

An audio-system proxy is not one of the four v1 roles. ARC/eARC and system-audio bridging require a separate capability design and hardware/interoperability gate. Ordinary directed volume keys and audio-status queries to a real AVR may be supported without claiming an audio-system role or promising a return-audio path.

## 3. Bind operations to real identities

The [domain model](03-domain-model-and-addressing.md) persists native equipment names as `QualifiedDeviceName`: consortium, Octopus, tentacle, side, and `osd_name`. `ComponentId` is an internal reference to the record holding those names. A native appliance binding's `device_name` selects one stored qualified name; discovery resolves it to a current `FullyQualifiedCecAddress`. The latter is a runtime endpoint, never the persistent device name.

Every actuation names the exact `BindingId`, `ComponentId`, resolved runtime endpoint, owner, and current binding/domain revisions. The owner must equal the endpoint's `octopus_id`; a native binding's `device_name` must match that endpoint's attachment. Ordinary unambiguous OSD-to-LA changes refresh the runtime binding automatically as specified in [discovery](05-discovery-and-topology.md). The compiler must not retarget stale queued operations by substituting a newly resolved LA; fresh work uses the new binding revision without requiring a new activity selection.

For example, `ensure_power` targets the TV's appliance binding. The compiler sends from a locally owned playback identity to the TV's logical address; it never transmits as the TV. `announce_active_source` targets a locally owned proxy binding representing the chosen source and broadcasts that proxy's verified physical address. Logical address 15 exists only in the compiled frame's broadcast/unregistered context, never as a qualified equipment target.

A proxy binding must not contain `device_name`: its advertised OSD name is role configuration, not a native equipment selector. An AVR-output proxy can represent different selected receiver sources without changing its physical LA/PA. Give each alternative source representation its own binding, referring to one shared proxy-identity `ResourceId`. Allocate that identity once and permit only one active representation for the given domain/path; compatible sessions may share that representation. Activating source Y replaces source X's grant, not its native appliance identity. Schemas must allow these deliberate alternative proxy bindings while rejecting simultaneous contradictory allocations.

Use semantic operations, not externally supplied byte arrays:

```cpp
struct AuthorizedCecOperation {
    CommandId command_id;
    OperationKind kind;
    ComponentId component_id;
    BindingId binding_id;
    FullyQualifiedCecAddress target; // runtime resolution or owned proxy allocation
    ExecutionFence fence;
    ValidatedOperationArguments arguments;
};

PolicyInstallResult install_policy(const PortPolicySnapshot&);
AdmissionResult execute(const AuthorizedCecOperation&);
void observe(PortHandle, const ValidatedFrame&);
void cancel(const ExecutionFence&);
void release_hold(const HoldToken&);
```

These are internal sketches, not an alternate wire protocol. [Transport](08-consortium-transport-and-routing.md) defines authenticated envelopes, deduplication and boot identity. `ExecutionFence` carries the applicable coordinator epoch, configuration/domain/binding revisions, session and route generation. `LocalCecAgent` rechecks the fence and local expiry at admission and immediately before transmission. The ISR operates only on already admitted local wire work.

Policy snapshots are immutable and bounded. They declare role, allocated identity, PA provenance, allowed binding capabilities, active route grants, permitted quirks, and policy generation. Installing a snapshot does not itself power equipment or select a route. Replace the current `Hub::configure()` standby side effect with staged policy/configuration application.

## 4. Compile only explicit semantic actions

The following table defines the default compiler boundaries. Exact operation names and argument encoding are owned by the transport schema; the wire effects below are normative for the proposed CEC capability.

`ensure_power` requires a verified discrete, idempotent power capability. A power toggle is never an implementation of desired-state reconciliation, even when a recent observation suggests its likely effect. If supported, it is a separately requested one-shot key action whose uncertain outcome is reported and not automatically replayed.

| Authorized action and target binding | Local CEC effect | Required restriction |
| --- | --- | --- |
| Ensure TV/display power, appliance binding | `Image View On` (`0x04`) from the local playback proxy, or a qualified device-specific operation. | Correct target, current plan, bounded retries; no inferred success from ACK alone. |
| Ensure other equipment power, appliance binding | Qualified explicit power-on/off key sequence or directed `Standby` (`0x36`). | Never substitute a toggle when its state uncertainty would invert the desired outcome. |
| Select receiver/source path, routing appliance binding | `Set Stream Path` (`0x86`) from the designated TV-controller identity with the path valid in that domain, or a qualified exact-input operation. | Target input resource reserved by the plan; broadcast is a compiled effect, not an equipment identity. |
| Announce active source, proxy binding | `Active Source` (`0x82`) with the local proxy PA. | Grant matches the domain, binding and route generation; no unresolved/fallback-unconfirmed PA. |
| Press/release a key, appliance binding | `User Control Pressed` (`0x44`) and matching `User Control Released` (`0x45`). | Correct held-key token and local lease; press cannot migrate between bindings. |
| Query a bound appliance | Approved identity, power or audio-status request. | Expected reply type, bounded rate, no implicit route mutation. |

Selecting a source behind an AVR usually requires coordinated effects on more than one domain. A TV-controller identity on the AVR domain can select the appropriate verified source path. The chosen source's front proxy can then announce itself on that domain. An AVR-output front proxy announces the correct path on the TV domain. These are separately authorized operations in one plan, not frame copies. An unused sibling proxy must not answer as active merely because another display has an active session.

A binding's role identifies its local executing port, not the remote appliance's CEC device type. Seeing the same AVR through a playback-proxy port does not give that observation the TV-controller routing capability. In v1, selecting a display's attached HDMI input uses that input's playback proxy and its authorized active-source sequence; do not silently compile a TV-origin `Set Stream Path` from an arbitrary playback address.

Whether two displays can use different sources through the AVR depends on its verified input/output resources. CEC proxies do not create an independent matrix. Likewise, output enable/selection absent from a receiver's CEC interface requires a separately qualified control binding or a user action. The planner must reject unsupported combinations before sending any partial route. See [activity planning](07-activity-planning-and-execution.md).

## 5. Received-message policy

Parse frame lengths, operand ranges, claimed initiator and destination, and opcode-specific context before producing an event. Device text is untrusted display data. A valid CEC frame is neither authenticated user intent nor a consortium command.

| Received traffic | Local handling | Cross-domain handling |
| --- | --- | --- |
| Presence poll to our owned address | ACK locally; no application operation. | None. |
| Identity request to our owned identity | Return the configured role's truthful supported identity, PA and feature set. | None; never copy the other side's entire identity. |
| `Report Physical Address`, OSD/vendor/version, power/audio status | Produce scoped observations after validation. | Send bounded semantic observations to the coordinator; no frame forwarding. |
| `Request Active Source` (`0x85`) | Reply only from the proxy currently authorized as active for that domain. | None; receiving the query never grants activity. |
| Source-originated `Active Source`, `Image View On`, `Text View On`, routing messages, or standby | Record/suppress; optionally notify the coordinator as an observation. | No re-emission and no reactive input restoration. |
| `Set Stream Path` or routing changes from a real display/AVR | Observe against the current plan; an expected selection may trigger only its already-authorized proxy response. | Unexpected/manual changes become planner events, not automatic new grants. |
| Keys directed to an owned proxy from a configured CEC control surface | Convert approved key types to an input event associated with the current session. | Route through `SystemControl/GroupService`; do not copy the original frame header. |
| Unknown or vendor-specific commands | Ignore or process through an explicitly qualified local quirk. | Deny forwarding by default. |

The suppression set includes `0x04`, `0x0D`, `0x36`, `0x80`, `0x81`, `0x82`, and `0x86`, and any other source message that would acquire routing or power effects if translated. This is not a blacklist-based bridge: **all cross-domain effects require an allowed semantic operation**, including unfamiliar vendor commands. Source metadata must never invoke the baseline's discovery-driven `input(current())` behavior.

Qualify `Request Active Source` and `Set Stream Path` handling explicitly; merely retaining the current receive switch is insufficient for several virtual sources. A proxy may answer an expected stream-path selection only when the current plan has already authorized that path. A real TV's own remote can change the TV input locally before Amity observes it; the source-isolation guarantee does not prevent that user action. The planner's manual-override policy decides what to do next.

Identity replies must advertise only implemented features. Power reports should use the bound component's valid observation or a documented virtual-role state, with unknown represented honestly. Network loss is not evidence that a display powered off. Avoid declaring every proxy “on” because some unrelated activity is active, as the single-hub model can effectively do.

## 6. Audio and quirk boundaries

The baseline has directed AVR volume and audio-status handling. Preserve these as explicit capabilities with real appliance targets. Do not automatically mirror `System Audio Mode Request`, system-audio broadcasts, ARC initiation/termination, or vendor audio negotiation across isolation boundaries.

ARC traditionally depends on CEC discovery/control; eARC has a separate data channel, while equipment may still use CEC for system-level power/volume behavior. These differences require testing the actual pass-through and devices, not assuming that cutting CEC proves every eARC mode impossible or that transparent conductors guarantee it works. See the [HDMI eARC overview](https://www.hdmi.org/spec21sub/enhancedaudioreturnchannel) and [Lattice ARC/eARC comparison](https://www.latticesemi.com/en/Solutions/Solutions/Standards/HDMI21earc). Return-audio activities remain unavailable unless the separate capability gate passes.

A quirk is a bounded, versioned recipe attached to a component/model and capability. It specifies allowed outbound operations, delays, reply expectations and retry bounds. It may not introduce arbitrary network-to-CEC transmission, impersonate an appliance address, bypass stale-work checks, or turn an unsolicited source frame into a route grant. Unsupported quirks fail visibly and preserve the source isolation gate.

## 7. Holds, route transitions and loss of authority

Held-button state and message decisions belong to the emitting executor. The token binds the original controller's local input context, session, route generation and target binding. Devices communicate down/up state changes only; each key has an independent completion-paced continuation. New commands submit immediately without waiting for any ACK; keyed up stops only that continuation and already admitted frames finish. Local and remote owners use the same `main` CEC behavior. The user's explicit amendments require one wire attempt per key-press frame. A failed TX ends that key's pressed state and repetition; later matching button-up is a no-op and unchanged down or general authority renewal cannot recreate it. The executor derives global release from the end of the receiving device's press stream, including last-key failure, only when all its keys are no longer pressed. Release has no key operand; aggregate by physical wire, initiator and destination, independently of other recipients. There is no per-button lease or renewal traffic. Current authenticated link/plan authority and exact boot/binding/route fences must remain valid for further attempts. The initial delivery deadline gates the first press, not the duration of an intentional hold. The executor has no transmit queue: only CEC buffers outgoing frames, in a FIFO with eight total messages per wire, and sends exactly what the executor supplies without opcode/button policy. [Execution contract](07-activity-planning-and-execution.md#7-keys-and-remote-input).

A matching button-up/cancel remains admissible despite ordinary semantic overload. It changes only its own live key state, never a newer hold that reused the same opcode or target; up for an ended failed key is a no-op. Do not send release while another key remains pressed for that recipient. At eight admitted messages, drop and log the final release, consume its cleanup decision and do not retry it from executor state. Log every TX dropped because the FIFO is full, including every frame of a rejected atomic batch. General authority/liveness monitoring must run during scans, slow TLS operations and network failure. Stopping repeats alone is not evidence that equipment received a release.

On a route change, atomically close admission for the old affected generation, invalidate its queued operations, release matching holds and wait for the bounded outcome of in-flight frames before granting a conflicting route. Automatic `Active Source` replies inherit the same route grant and must pass the same generation checks as explicitly requested announcements. A barrier cannot acknowledge quiescence while leaving a path that admits a new old-generation reply. A frame already on the wire cannot be “unsent.” Publish applied/uncertain results separately from observed equipment state. After a crash around a non-idempotent operation, do not replay it as though exactly-once delivery were guaranteed.

When authority expires or the coordinator becomes unavailable, retain local source isolation, basic identity service where still valid, and hold cleanup. Stop new route/power actuation and new active-source announcements requiring expired grants. Do not elect a new coordinator locally or send standby to equipment still being watched. Removal, recabling quarantine and retirement use [fault and lifecycle rules](13-faults-removal-and-recovery.md).

## 8. Implementation work and acceptance criteria

Extract role-specific identity and message handling from [hub.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp) into host-testable policy/compile functions. Reuse validated frame parsing, power/key recipes and existing hold-generation lessons. Replace global bare-name lookup with the scoped `QualifiedDeviceName` resolver; remove single-activity assumptions, automatic theft repair, and discovery-triggered routing. Wire the functions into one `LocalCecAgent` over the shared runtime; do not instantiate four independent `HubService` activity controllers.

Acceptance must include wire capture as well as unit tests:

- An isolated unselected source sends each activation/routing/standby opcode, malformed variations, unknown vendor messages and bursts. Capture **zero prohibited re-emissions** on every AVR/display domain, including during boot, scan, network loss and retirement.
- The selected source can still receive power/key operations and establish a planned route through the AVR to each supported display. Source-native and proxy bindings produce the correct distinct wire initiators and physical addresses.
- Multiple proxies share a domain without duplicate ACK ownership. Only the authorized active proxy replies to `Request Active Source`; all can answer their own identity queries.
- A stale binding, domain, configuration, epoch, boot or route generation is rejected at queue admission and again before wire start. Delayed responses cannot resurrect a superseded active source.
- A native qualified name resolves automatically to its new LA on the same expected attachment/domain. Fresh commands use the new binding revision; queued work is not retargeted, no source-selection frame is emitted by discovery, and a same-name proxy is never mistaken for the native device.
- A full CEC FIFO, slow discovery and dropped network packets cannot block button-state processing or general authority fencing; a final release at the eight-message limit is dropped and logged without an admission retry, and an old release cannot release a newer hold.
- Discovery or configuration staging produces no standby, wake or routing frame. Equipment state is not reported changed solely because transmission completed.
- Unsupported ARC, output routing, identity capacity and quirks fail before partially executing an activity. An unisolated source is shown as outside the full source-suppression guarantee.

The policy owner maintains the opcode/role test matrix. The hardware validation owner supplies multi-domain captures, and the planner owner verifies resource and transition tests in [the release plan](15-validation-and-release-gates.md).
