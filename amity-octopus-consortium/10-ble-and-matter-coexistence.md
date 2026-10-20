# 10. BLE and Matter coexistence

## Purpose and baseline

This proposed contract integrates consortium enrollment with the existing Matter server and BLE remote. It does not add a second Bluetooth host, a second Wi-Fi manager, or an ESP-Matter commissioner. Enrollment security is defined in [09](09-membership-security-and-enrollment.md); [20](20-user-experience-and-guided-setup.md) presents the end-to-end user journey. This document remains authoritative for radio ownership, physical entry, and Wi-Fi integration.

At Amity `d50af8026938797fc0820d0107f4c4e064c2d2bc`, Matter initializes NimBLE and station networking. The BLE remote starts afterwards and consumes that shared host. The configuration keeps BLE after commissioning, enables central/observer/GATT-client roles, permits three simultaneous connections and four bonds, and disables SoftAP. `ble_remote` manages one remote and can start an indefinite reconnect initiation. These facts are visible in pinned [startup](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/main/main.cpp), [Matter startup](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_matter/amity_matter.cpp), [BLE remote](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/ble_remote/ble_remote.cpp), and [sdkconfig defaults](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/sdkconfig.defaults).

The SDK versions are pinned in [SDK_VERSIONS.txt](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/SDK_VERSIONS.txt): ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643`, ESP-Matter `59574f3fb62fddc690b4127fd3a4a43cfce4245e`, and ConnectedHomeIP `539342f32d5f4dc93761c2f9325afe29270068f1`. Agents must verify these integration points again when changing a pin.

## Ownership and invariants

Matter remains the lifetime owner of NimBLE, Wi-Fi, the station netif, and CHIP event processing. A new application `BleRadioArbiter` owns admission and scheduling of Amity's high-level workflows. It does not replace Matter's host reset/synchronization callbacks. A `ConsortiumGattTransport` supplies one additional service, and a `WifiEnrollmentAdapter` calls the existing CHIP network driver.

The following are invariants:

- Starting or finishing a workflow never deinitializes NimBLE, releases Bluetooth controller memory, or clears all bonds.
- An established remote connection may remain active during enrollment or Matter induction. Its input and release path must continue to meet [07](07-activity-planning-and-execution.md)'s hold deadlines.
- Remote pairing, remote reconnect initiation, consortium scanning/connection, and connectable advertising are explicitly scheduled; modules do not compete by issuing uncoordinated GAP calls.
- No enrollment service accepts a PAKE session outside its physically authorized window. A registered GATT table is not an open enrollment window.
- All radio work is bounded. Local CEC timing, filtering, and release handling continue even while crypto, Wi-Fi, or BLE fails.

Matter fabric membership, Wi-Fi provisioning, consortium membership, and BLE remote bonding are four separate states. Removing the last Matter fabric does not remove a consortium, erase its Wi-Fi, grant a new owner control of it, or automatically reopen either setup window.

## Register the extra GATT service before host startup

The pinned ESP32 NimBLE implementation exposes a supported hook:

```cpp
CHIP_ERROR ConfigureExtraServices(
    std::vector<struct ble_gatt_svc_def> &extGattSvcs,
    bool afterMatterSvc);
```

Call `chip::DeviceLayer::BLEMgrImpl().ConfigureExtraServices(...)` before `esp_matter::start()`, once per boot. Keep the service definition, characteristic arrays, UUIDs, and callback context alive for the entire host lifetime. Supply the extra service definitions without a premature terminating service; the hook adds the final terminator.

The boolean name is misleading at this exact pin: `false` produces Matter followed by the extra services; `true` places the extras before Matter. Check the implementation, not the parameter name. Calls after the internal service vector has been populated return `CHIP_ERROR_INCORRECT_STATE`. NimBLE initialization later counts/adds the combined services before starting its host task. See the pinned [public hook](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/platform/ESP32/BLEManagerImpl.h) and [service construction and initialization](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/platform/ESP32/nimble/BLEManagerImpl.cpp).

Do not append services with an ad hoc `ble_gatts_add_svcs()` after the running host has registered its table. Do not invoke `protocomm_ble_start()` alongside Matter: the pinned protocomm NimBLE transport initializes a host, installs callbacks, and starts its own task. Reuse the protocomm session/security/request functions through the custom transport instead. See pinned [protocomm NimBLE ownership](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/protocomm/src/transports/protocomm_nimble.c).

Map an admitted connection to exactly one `protocomm_open_session` / `protocomm_req_handle` / `protocomm_close_session` lifecycle. A second unsolicited connection cannot replace the active session. The enrollment service does not require a persistent BLE bond: Security2 authenticates and encrypts its application records. This avoids consuming the remote's bond budget or weakening global security settings to accommodate enrollment.

GATT callbacks perform bounded validation/copying and enqueue work. SRP, GCM, protobuf parsing, flash commits, and Wi-Fi transitions run in workers. Support the negotiated ATT MTU, including the minimum; the existing preferred MTU of 527 is not a guarantee. Fragment whole Security2 records with bounded offsets, transaction/connection generation, and duplicate detection. Never hand individual fragments to the crypto layer.

## Boot policy must close two independent automatic paths

An active consortium member may deliberately have no Matter fabric. Boot must not make such a board perpetually commissionable.

1. Before Matter start, supply a persistent `chip::CommonCaseDeviceServerInitParams` through `esp_matter::set_server_init_params()`, with `advertiseCommissionableIfNoFabrics=false`. The pinned ESP-Matter core performs `InitializeStaticResourcesBeforeServerInit()` on the selected object and supplies its default AppDelegate only when none is provided. The object must outlive asynchronous server initialization. See pinned [ESP-Matter initialization](https://github.com/espressif/esp-matter/blob/59574f3fb62fddc690b4127fd3a4a43cfce4245e/components/esp_matter/esp_matter_core.cpp) and [Matter no-fabric window](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/app/server/Server.cpp).
2. Set `CHIP_DEVICE_CONFIG_CHIPOBLE_ENABLE_ADVERTISING_AUTOSTART` to false through the project CHIP configuration. The pinned BLE manager independently initializes its advertising-enabled flag from this macro. Disabling the server's automatic commissioning window alone is insufficient. Keep `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n`; suppressing an advertisement is different from destroying the shared host.

After host synchronization, the application opens only the workflow justified by durable state and physical authorization. The first-board Matter setup experience remains available through the explicit gesture policy below; an upgrade must not silently reopen setup on an established device. An eligible fresh/unconfigured singleton can enter first-board setup, while an OCTOPUS with a consortium record and no fabric starts its existing network and operational service with enrollment closed. Absence of a network or fabric alone is not setup authorization. Pending, retired, or recovery-required records follow [09](09-membership-security-and-enrollment.md) and [13](13-faults-removal-and-recovery.md).

The current `amity_matter_open_commissioning()`/close wrappers become arbiter clients. Also observe commissioning initiated by an external administrator over an existing Matter fabric; gating only the web buttons is incomplete.

### Proposed retail-profile physical entry

The following is a proposed requirement for supported button-equipped retail profiles, not behavior supplied by the baseline firmware. Evaluate a debounced press as one action; do not enter a shorter-duration mode while a longer press remains held.

| Physical action | Permitted request |
| --- | --- |
| Release before 3 seconds after debounce | Open the bounded initial Matter setup window only for a fresh/unconfigured singleton with no active consortium membership, prepared join, existing operational configuration, or conflicting arbiter session. |
| Release after at least 3 seconds and before 10 seconds | Open the 180-second consortium enrollment window only when [09](09-membership-security-and-enrollment.md)'s bootstrap and physical-authorization prerequisites and the arbiter permit it. |
| Hold reaches 10 seconds | Follow the factory-reset path; neither shorter setup mode may have opened during that hold or open on its eventual release. |

Creating an origin identity or an otherwise unconfigured singleton does not itself open a window. In configured, pending, retired, or recovery states, the short gesture does not silently commission the device; use the existing authorized management/recovery path. Reboot, window expiry, and removal of the last Matter fabric never substitute for a new authorized setup action. Conflicting sessions return busy under the arbiter contract rather than admitting competing advertising.

Each hardware profile must supply the exact gesture in its printed and browser instructions, define indicator behavior only for indicators it actually supports, and retain the USB-assisted physical path for buttonless legacy hardware. Qualify debounce and releases below/at/above 3 seconds and 10 seconds, a hold continuing through both thresholds, release after reset admission, and state/arbiter eligibility. These checks must prove that one press cannot open both Matter and consortium setup or bypass a prepared/active membership.

## Radio arbiter contract

The arbiter maintains an owner, local monotonic deadline, monotonically increasing in-memory generation, active connection handles, and pending cancellation state. Callers receive a reservation or an explicit busy result. Late callbacks carry their generation and may clean up their old resources, but may not advance the replacement workflow.

| Mode | Discovery/advertising owner | Admitted new work | Existing remote |
| --- | --- | --- | --- |
| Operational | None, or scheduled remote reconnect | Routine reconnect when no setup reservation exists | Continues |
| Matter induction | Matter commissioning manager | Matter session only; new consortium/remote pairing requests deferred | Continues if resources permit |
| Remote pairing | Remote central/scanner | One user-authorized remote pairing attempt | Existing bond replacement is explicit |
| Consortium candidate | Amity connectable advertisement | One physically authorized Security2 server session | Continues |
| Consortium inviter/relay | Amity central/scanner | One selected candidate connection | Continues |
| Transition/cancel | Previous owner until cleanup completes | No new GAP operation | Continues where possible |

An operational CASE connection over Wi-Fi does not consume a BLE connection. Budget at most one connected remote plus one active setup connection for normal v1 operation, leaving the third configured slot for bounded transition/error handling. Do not assume another advertising set exists, increase connection counts without memory/timing measurements, or consume four bonds for consortium members.

Use these initial acceptance targets: a 180-second enrollment reservation, a 20-second discovery attempt, a 10-second connection attempt, and a 5-second cancellation completion deadline. Existing explicit remote scan/pairing behavior may retain its shorter user-visible limits. A timeout reports a bounded failure; it does not reset the shared host to make progress. These are proposed policy constants to validate, not measured guarantees.

Before an inviter scans/connects, suspend the remote's saved-peer reconnect procedure. The baseline invokes `ble_gap_connect(..., BLE_HS_FOREVER, ...)`, so “the remote is currently disconnected” does not imply GAP is free. Invalidate its attempt token, request `ble_gap_conn_cancel()`, and wait for completion before granting the reservation. Likewise cancel a remote scan before a conflicting scan. Preserve the remote bond and restart scheduled reconnect after releasing the reservation. The existing generation-based cancellation logic is the starting point; expose it through the arbiter rather than duplicating hidden GAP ownership.

Before candidate advertising, close/disable Matter advertising through the Matter API and observe the stopped state. Only then install the Amity advertisement and start it with the existing host. On exit, stop the Amity advertisement, finish session cleanup, and restore whichever authorized workflow the arbiter admits next. `ConfigureScanResponseData()` changes Matter's advertisement data; it is not an independent Amity advertising owner.

### Externally initiated Matter commissioning

An active Matter PASE/fail-safe operation has priority and is never preempted by a consortium join. A newly opened external Matter window cancels an uncommitted consortium radio workflow and defers remote pairing. Committed pending enrollment can later resume over its pinned LAN channel.

The pinned `CommissioningWindowManager::StartAdvertisement()` calls `SetBLEAdvertisingEnabled(true)` before `OnCommissioningWindowOpened()`. The AppDelegate callback is therefore an observation hook, not an admission veto. Use a synchronous integration adapter to stop Amity advertising/initiation and invalidate its generation before a competing host procedure can start. Preserve the default ESP-Matter delegate's four posted session/window events when installing a custom delegate; add establishment/fail-safe tracking rather than suppressing those notifications. See pinned [commissioning order and callbacks](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/app/server/CommissioningWindowManager.cpp).

This ordering is a mandatory hardware integration gate. If the pinned stack's task scheduling cannot guarantee race-free handoff, add a narrow reviewed pre-advertisement admission hook in the tracked SDK integration, with regression coverage. Do not ship simultaneous competing advertisement owners or pretend the post-open callback itself vetoes Matter. Keep the protocol-level commissioning behavior valid and preserve an ongoing PASE session.

## Wi-Fi remains owned by CHIP without a Matter fabric

The existing Matter stack may own station networking even when `FabricCount == 0`. Consortium enrollment must not call a second `esp_wifi_init()`, a separate provisioning manager, or SoftAP. Create an application adapter around the pinned `ESPWiFiDriver` on the CHIP task/under its expected lock. Its public API includes `AddOrUpdateNetwork`, `ConnectNetwork`, `CommitConfiguration`, and `RevertConfiguration`; none requires inventing a Matter fabric. See pinned [driver interface](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/platform/ESP32/NetworkCommissioningDriver.h) and [driver persistence and connection logic](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/platform/ESP32/NetworkCommissioningDriver.cpp).

The driver has one staged network. It backs up configuration during change, and initialization reverts an uncommitted backup. A successful station connection is consequently not proof that new credentials survive reboot. Its configured connect timeout is 30 seconds; the adapter reports that stage accurately instead of using the 10-second BLE-connect timeout.

For a previously unconfigured candidate, apply this transaction:

1. Require a mutually authenticated Security2 session and matching prepared join. Serialize against Matter network commissioning and its armed fail-safe.
2. Supply SSID/credentials through the existing driver, connect, and wait for its result plus a usable IP address. Never log credentials or embed them in portable configuration.
3. Persist the pending enrollment record as described in [09](09-membership-security-and-enrollment.md). Call `CommitConfiguration()` once the intended Wi-Fi connection succeeds, before reporting the network step durable. Maintain journal markers so restart can distinguish an SDK rollback from an already committed network.
4. Perform the separate mTLS LAN proof and membership commit. If it fails, the candidate may remain on the successfully provisioned network with pending/no membership. Explain this state in the UI; do not claim an atomic cross-device Wi-Fi/membership transaction.

An interrupted uncommitted network change is reverted through the driver. A restart must reconcile the journal with the actual driver state before claiming network readiness; if necessary, the already prepared transaction can reapply its retained, protected credentials, or ask the owner to resume. It must not silently reopen unauthenticated provisioning.

For an already networked candidate, retain its network and attempt the pinned LAN proof. Different SSIDs can reach the same LAN; SSID equality is not membership proof. The pinned driver's single-network behavior can reject a different staged SSID, so do not work around that by clearing configuration secretly. Network migration is a separate explicit operation that also respects Matter fail-safe state. V1 credential transfer should target the network modes actually supported and tested by the existing station build; do not promise enterprise credential cloning.

## Why not commission an internal Matter fabric in v1?

The pinned ESP-Matter Kconfig makes `ESP_MATTER_COMMISSIONER_ENABLE` depend on `!ESP_MATTER_ENABLE_MATTER_SERVER`. Amity is already a Matter server. A controller/server example does not establish commissioner/server support, nor does it prove sufficient memory alongside HomeKit, remote BLE, and ten CEC engines. See pinned [controller Kconfig](https://github.com/espressif/esp-matter/blob/59574f3fb62fddc690b4127fd3a4a43cfce4245e/components/esp_matter_controller/Kconfig).

V1 therefore retains Matter for external home integration and uses the separate consortium protocol. An internal Matter fabric remains a future architecture spike requiring a working combined build and resource/upgrade/security evidence. Ordinary Matter group membership also does not define Amity's resource graph, enrollment authority, or coordinator transfer semantics.

## Integration tasks and acceptance gates

Implement startup gating in `amity_matter`, arbiter-mediated GAP operations in `ble_remote`, the extra pre-start GATT service, the Security2 worker transport, and the CHIP Wi-Fi adapter. Wire durable state into startup before `esp_matter::start()`. Preserve the existing remote's bond-specific unpair behavior and Matter fabric management.

Release gates include: extra service visible on the first boot without a second host; no automatic Matter window/advertisement after reboot of a consortium-only member; remote reconnect cancellation followed by successful enrollment; connected remote key release under SRP and Wi-Fi load; minimum-MTU fragmentation; three-link exhaustion returning busy; external Matter window races; interrupted Wi-Fi apply/commit/restart; removal of the last Matter fabric without membership loss; and memory/CEC-timing stress with the full ten-line hardware profile. [15](15-validation-and-release-gates.md) consolidates the release matrix. Failure of a gate blocks the conflicting feature combination rather than weakening authentication or CEC timing.
