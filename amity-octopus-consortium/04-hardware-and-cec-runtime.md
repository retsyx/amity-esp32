# Hardware and CEC runtime

**20 October 2026 controller-policy amendment:** [Original controller and subordinate devices](21-original-controller-policy.md) is normative. The original first-setup Amity alone hosts the UI/integrations and sole remote; added devices are UI-less actuators, never input gateways or successor controllers.

**21 October 2026 algorithm-preservation amendment:** scaling the runtime must preserve original CEC/remote algorithms and functionality unless the user explicitly requests a change. The current `main` implementation is authoritative (`98e64a6` for this merge). Each key has its own completion-paced continuation; new keys and commands submit immediately while earlier ACKs are pending, with no timer or queued repeat burst. All profiles use the same dispatcher, wire engine, eight-plus-one FIFO and receive capacity. Distribution only moves messages and must have zero effect on CEC behavior. Reporting more precise wire outcomes must not change those rules. Necessary distributed fences are recorded separately. [Normative rule](21-original-controller-policy.md#original-algorithms-and-functionality-amendment--21-october-2026).

**Status: proposed implementation specification v0.1.** This document defines the local electrical and timing foundation for an OCTOPUS with one to five TENTACLES. It does not claim that the existing two-port hardware or firmware has passed ten-line qualification. The source baseline is Amity-esp32 commit `d50af8026938797fc0820d0107f4c4e064c2d2bc`.

## 1. Responsibilities and boundaries

One TENTACLE is one HDMI pass-through with two separated CEC conductors. Its `front` port faces the HDMI sink; its `back` port faces the source. Consequently, five tentacles require **ten independently timed CEC engines and ten suitable GPIO connections**. Orientation is a physical property, independent of the port roles defined in [CEC roles and message policy](06-cec-roles-and-message-policy.md).

The runtime owns open-drain GPIO operation, frame timing, arbitration, ACK generation, bounded transmit queues, receive observations, and local completion results. It does not own activities, membership, equipment identity, or routing decisions. `LocalCecAgent` installs authorized policy and submits operations; the runtime executes local wire work. Neither LAN latency nor coordinator availability may determine a CEC ACK bit.

The explicit [executor/CEC amendment](21-original-controller-policy.md#executor-and-cec-boundary-amendment--21-october-2026) requires one TX FIFO per CEC wire in every profile, including legacy: eight normal messages plus one release-only overflow message, counting queued and active work together. Ordinary messages reject at eight; one executor-selected release may occupy slot nine, and further releases at nine drop without executor admission retry. This is the only outgoing-frame buffer. The executor owns button state and all message/repeat/release choices, with no TX queue. CEC is a mechanical sender: it obeys supplied frames, attempt budgets, generic overflow permission and guards; it must not inspect a key opcode to choose policy or synthesize messages. The executor supplies one wire attempt for every key-press frame. Failed key TX ends that key's pressed state and repetition, so later button-up is a no-op. The executor attempts implicit release at the end of a receiving device's press stream, including last-key failure, only when all its keys are no longer pressed.

The existing PCB separates HDMI pin 13 into `/HDMI_CEC_0` and `/HDMI_CEC_1`. Other HDMI paths, including DDC and HPD, are passed through. The existing mapping uses GPIO1 and GPIO2 for CEC and GPIO4 for the reset button. These are facts about the current board, not a five-tentacle pin assignment. See the pinned [PCB](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/hw/board/hdmi.kicad_pcb), [schematic](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/hw/board/hdmi.kicad_sch), and [board pins](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/main/board_pins.h).

The CEC cut remains when the board is unpowered: there is no bypass relay. This provides no galvanic isolation, because HDMI grounds and other conductors remain connected. Product instructions must distinguish “HDMI continues through the connector” from “CEC continues through the connector.”

## 2. Hardware profiles are a release gate

Introduce a compiled, versioned `BoardProfile`, selected independently of portable system configuration. Its immutable tentacle indices are `0..4`; user-facing labels are 1–5.

```cpp
struct PortPin {
    TentacleId tentacle_id;
    Side side;                 // front or back
    int gpio;
};

struct BoardProfile {
    BoardProfileId id;
    uint32_t revision;
    uint8_t tentacle_count;     // 1..5
    std::array<PortPin, 10> ports;
    OptionalPin reset_button;
    OptionalPin status_led;
};
```

Only `2 * tentacle_count` entries are active. Startup validation must reject repeated GPIOs, invalid output GPIOs, pins assigned to another enabled peripheral, mismatched connector indices, and a profile unsupported by the firmware. Invalid profiles leave every available CEC output released and report a local hardware fault; they do not partially activate a guessed mapping.

The standard XIAO ESP32S3 exposes eleven general-purpose connections, making ten CEC connections plus a button superficially attractive. The actual choice must account for strapping pins, boot ROM behavior, UART/USB use, flash/PSRAM wiring, LEDs, and any fixture connections. GPIO quantity alone does not establish electrical suitability. A custom carrier or module profile is a reasonable five-tentacle candidate; it remains a hardware design decision. Consult the [Seeed pin documentation](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/) and [ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf).

| Gate | Owner | Evidence required before approving a profile |
| --- | --- | --- |
| GPIO allocation | Hardware engineer | Schematic and boot/reset measurements demonstrating released CEC outputs, no push-pull conflict, and correct button behavior. |
| CEC loading | Hardware engineer | Pull-up, capacitance, voltage, leakage, ESD, powered and unpowered measurements, including multiple ports attached to one external domain. |
| HDMI pass-through | Hardware engineer | Signal-integrity and interoperability evidence for the advertised cable lengths, resolutions, refresh rates, and HDMI features. |
| Runtime resources | Firmware engineer | Ten-line timing, internal memory, interrupt and task-stack measurements with the final radio and OTA configuration. |

The existing 27 kΩ per-port pull-ups must not simply be multiplied without evaluating the resulting shared-domain load. I²C GPIO expanders are not an acceptable replacement for directly timed CEC pins in this design.

## 3. Replace per-port timers with one shared scheduler

The current `cec::Bus::Impl` allocates one GPTimer and one 3072-byte transmit task per wire. Its timer runs at 1 MHz with a 50 µs repeating alarm. Sharing an interrupt input does not share the underlying hardware timer. See [bus.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/cec/bus.cpp).

ESP32-S3 has two timer groups with two general-purpose timers each: **four GPTimers, not ten**. The official, version-pinned [ESP-IDF v5.5.5 capability header](https://github.com/espressif/esp-idf/blob/v5.5.5/components/soc/esp32s3/include/soc/soc_caps.h) defines `SOC_TIMER_GROUPS` and `SOC_TIMER_GROUP_TIMERS_PER_GROUP` as two each. This hardware limit also appears in the datasheet; the project itself pins a newer development SDK in [SDK_VERSIONS.txt](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/SDK_VERSIONS.txt). The implementation must compile against that project SDK and assert the target capabilities instead of downgrading it to match this reference.

Create one `CecRuntime` and retain up to ten allocation-free `cec::Engine` instances. Its default schedule is one 50 µs hardware alarm. The ISR performs a fixed, bounded pass:

1. Capture one monotonic timestamp and the required GPIO input-bank snapshots.
2. Tick every enabled engine against that timestamp and its captured input level.
3. Apply the resulting open-drain low/release changes after all input levels have been sampled.
4. Copy completed observations and transmission results into bounded internal-memory slots; notify the worker once.

Sampling all lines before changing outputs prevents software iteration order from inventing different observations when two local ports are electrically on the same external domain. GPIO-bank reads cannot be physically simultaneous; their bounded skew must be measured. Domain scheduling should avoid unnecessary local self-contention, but ordinary arbitration remains mandatory.

The ISR must not allocate, log, parse JSON, perform TLS work, access PSRAM-only data, wait on a task mutex, or invoke flash-resident helpers. Review the entire called code path and linker map. The current engine deliberately uses 32-bit division during transmission to avoid a flash-resident 64-bit division helper; preserve that property. Espressif documents cache-safe GPTimer operation and memory requirements in its [v6.1 GPTimer guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/gptimer.html).

Missed time must be handled as a discontinuity, not by replaying synthetic ticks. The current [engine.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/cec/engine.cpp) invalidates receive state and releases an active transmission after a gap over 200 µs. Preserve its pulse validation, arbitration, ACK, free-time, frame-length, and stuck-line checks. Ten-line support must not be achieved by relaxing those checks.

## 4. Separate observation from ACK ownership

The current engine both ACKs its configured logical address and delivers only frames addressed to that address or broadcast. Shared-domain discovery needs to observe foreign directed traffic without claiming it. Add independent controls:

```cpp
struct ReceivePolicy {
    bool observe_all;                        // delivery, never ownership
    std::optional<CecLogicalAddress> ack_address;
};

struct ReceiveObservation {
    PortKey port;
    Frame frame;
    uint64_t local_rx_sequence;
    uint64_t local_monotonic_us;
    bool addressed_to_local_identity;
};
```

`ack_address` is either absent or `0..14`. Address 15 is never an owned identity. A monitor has no ACK address. A local playback proxy may observe all traffic while ACKing only its assigned address. A port continues to use one owned identity in v1; this change does not imply a multiple-address ACK mask or permission to impersonate other equipment.

Policy replacement must be atomic relative to frame reception. A role/address change waits for the applicable frame boundary before committing, or explicitly aborts and releases the line during a fault transition. It must not ACK the beginning of a frame as one identity and its end as another. Existing self-transmission suppression must remain distinguishable from observations made by a different local port on the same domain.

## 5. Asynchronous submission and bounded queues

Expose a narrow runtime API. Types referenced here are internal interfaces; network encoding belongs to [consortium transport](08-consortium-transport-and-routing.md).

```cpp
StartResult start(const BoardProfile&);
PortHandle port(TentacleId, Side);
InstallResult install_receive_policy(PortHandle, ReceivePolicy);
AdmissionResult submit(PortHandle, const WireOperation&);
void cancel_queued(const ExecutionFence&);
void service_completions();
void release_all_outputs();
RuntimeStatistics snapshot_statistics() const;
```

`WireOperation` contains a validated frame, local operation identifier, attempt bound, expiry and generic generation fences supplied by the executor. It is not an externally accessible arbitrary-frame API. The executor validates semantic policy before direct submission; CEC mechanically checks the supplied deadline/generation guards immediately before `Engine::begin()`, not only on queue insertion. It does not interpret membership, route, button or opcode meaning. A cancelled operation that has already begun must produce an explicit in-flight result; route transitions use the barrier in [activity execution](07-activity-planning-and-execution.md).

Admission copies messages directly into the single per-wire FIFO with eight normal slots plus one release-only overflow slot. Finite multi-frame recipes have atomic all-or-busy admission to that same queue; failed admission enqueues nothing. No executor queue may hold a remainder for later. Completion correlations and stable guard storage remain bounded by admitted work and survive until its terminal result; cancellation cannot acknowledge quiescence while a started frame remains active. End-of-stream cleanup is executor state, separate from ended key tokens, until the release decision can be ordered after its admitted presses. Attempt direct admission once: the release may occupy the ninth slot, but rejection at nine consumes/drops that decision with no later capacity retry. CEC never creates that release itself.

A nonblocking dispatcher should advance independent lines concurrently. Waiting up to a second on one wire must not stall the other nine. Keep per-line FIFO ordering within a priority class and bounded fairness between discovery and routine control. A domain-aware task layer serializes planned transactions on ports sharing a domain; the ISR still handles independent engines and local ACKs.

Maintain distinct outcomes: rejected admission, admitted, wire success, NACK, arbitration loss, timeout, line error, cancelled before transmission, and uncertain completion after restart. Wire success is not proof that equipment changed state. The baseline `send_and_wait()` reports queue admission and worker completion, not the actual CEC ACK outcome; callers must not preserve that ambiguity under a name implying success. See the existing [bus interface](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/cec/include/cec/bus.hpp).

Allocate queues and engine state once at startup in internal memory. Reserve completion capacity for every admitted operation. Keep button-up/cancel state admission and the local authority/liveness monitor independent of ordinary semantic overload, without adding another outgoing frame queue. Reducing ten transmit tasks to one dispatcher is recommended, but the final memory report must include all queues, stacks, snapshots, protocol buffers, and radio allocations; PSRAM capacity does not establish ISR memory headroom.

## 6. Lifecycle and failure behavior

Runtime states are `stopped`, `starting`, `running`, `quiescing`, and `faulted`. Start with outputs released and ACK ownership absent. Install only a validated profile, start the scheduler, then let discovery and policy assign identities. A partially failed start cleans up allocated resources and releases every configured output.

Quiescing rejects ordinary submissions, cancels stale queued work, services matching key releases, and waits for a bounded in-flight drain before disabling timing. A hardware or timing fault releases the affected line and reports its state without repeatedly resetting the entire Octopus. A shared scheduler failure releases all lines. No failure reconnects front CEC electrically to back CEC, and no network outage starts a transparent software bridge.

Record per-port receive drops, queue high-water marks, retry outcomes, arbitration losses, late ticks, maximum tick gap, and line faults. Record shared ISR execution time and internal-memory low-water marks without logging inside the ISR. Local monotonic timestamps are diagnostic within one boot; include `BootId` in exported events and do not compare them directly across Octopuses.

## 7. Implementation work and integration

1. Extract timer/GPIO ownership from `cec::Bus` into `CecRuntime`; preserve the pure engine and one-tentacle adapter during migration.
2. Add monitor-all delivery and explicit ACK ownership, with host tests proving their independence.
3. Replace blocking completion coupling with per-line operation state, actual `TxResult` propagation, generic cancellation guards and direct admission to the sole CEC FIFO with eight normal slots plus one release-only overflow slot. Keep release decisions in executor button state, with no additional frame buffer.
4. Replace fixed macros at the startup seam in [main.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/main/main.cpp) with the validated hardware profile. Expose one runtime through system services, not five independent activity hubs.
5. Connect `LocalCecAgent`, asynchronous scans, and policy installation only after engine regression tests pass.
6. Qualify each advertised hardware profile. If ten-line timing or GPIO qualification fails, reduce the supported profile or design a separately qualified hardware offload; do not label the unqualified configuration supported.

## 8. Acceptance criteria

The existing [host CEC tests](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_hub_test.sh) and [hardware fixture](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_hardware.py) are starting points. Passing the present two-wire fixture is insufficient for five tentacles.

- Parameterize engine/runtime tests over 2, 4, 6, 8, and 10 lines. Ten simultaneous maximum-length frames must complete without cross-port delivery, lost completion ownership, or serialized independent-wire operation.
- Prove foreign directed traffic is observed but never ACKed by a monitor. A locally addressed frame must be ACKed by exactly its owner; broadcast must retain the correct ACK behavior.
- Exercise simultaneous local transmission and external arbitration, receive-after-arbitration-loss, stuck-low faults, queue saturation, cancellation, malformed pulses, and the existing over-200 µs discontinuity behavior.
- Connect multiple local ports to one real electrical domain. Validate shared sampling, address ownership, and that domain scheduling never depends on port iteration order.
- On actual target hardware, capture waveforms on all ten lines while Wi-Fi, the original controller's BLE remote, Matter, HTTPS, discovery, and OTA flash writes run. Under the declared release workload, require protocol-valid waveforms, no unhandled timing discontinuities, no completion loss, and timely matching key release. Publish the workload, duration, maximum ISR duration and tick gap, and resource headroom.
- Test power-on, brownout, reset, initialization failure, and firmware restart electrically. No CEC pin may drive a conflicting high level, and no boot state may join the two CEC sides.

Detailed release ownership and test evidence are consolidated in [validation and release gates](15-validation-and-release-gates.md). Approval of the shared scheduler is conditional on this evidence, not on the nominal 50 µs period alone.

## Per-adapter settings presence and owner barrier

The explicit fixed legacy one-adapter profiles may report connector 0 present by construction. Development profiles must not borrow that fact: unconfigured pads and configured connectors without a qualified detector remain unknown, independent of CEC replies. Poll only an explicitly configured separate defined-state presence GPIO; do not enable CEC pull-downs, artificial detection pulses or a permanent weak-divider assumption. A removable-module pull-up or a floating idle CEC pin is not reliable positive presence evidence. Electrical qualification of any new detector remains required.

The removable-module pull-up can supply the positive signal on a **separate presence contact**: the module provides the pull-up, and the base provides a defined absent bias so unplugging it produces the opposite valid logic level. Keep this sense net independent of both HDMI-CEC wires. Qualify its voltage, leakage, thresholds, contact behavior and powered/unpowered states before selecting resistor values or enabling any sense-input bias. Configure that socket's `AMITY_TENTACLEn_PRESENCE_GPIO` and `AMITY_TENTACLEn_PRESENT_HIGH` to match the qualified circuit; any `PRESENCE_PULLUP`/`PRESENCE_PULLDOWN` applies only to that dedicated sense input. Internal slot `n=0` is displayed as **HDMI adapter 1**, through slot `n=4` / **HDMI adapter 5**. An unmapped or unreadable detector stays unknown and cannot unlock settings; the user does not manually mark an adapter present in the dialog.

Track each connector's cached optional presence and monotonic generation on the sole owner. Publish a change even when the device is idle/passive. Removal, unreadable detector and reinsertion fence only affected native bindings/first-edge work; reinsertion never replays activity or a hold. A Settings request captures the exact generation, and the owner re-reads the detector immediately before persistence so present→absent→present cannot pass as the original attachment. Unknown/absent settings are read-only; saved preferences retain their stable connector index.

The settings barrier rejects active sessions/holds, existing maintenance and pending configuration. It reserves positive admission, cancels discovery, drains started frame/completion and exact cleanup, rechecks BootId/generations/deadline, then invokes one owned bounded persistence callback. Waiting cancellation prevents that callback forever; once it starts, obtain its actual result. Durable success keeps the device quiescent for its explicit restart after the response transmission attempt. The five startup defaults and detected/selected diagnostic values confer no new CEC identity or route authority. [Settings acceptance](20-user-interface.md#per-device-hdmi-adapter-settings-amendment--20-october-2026).
