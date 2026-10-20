# 19 — Source baseline and evidence

**Reviewed:** 20 October 2026. **Repository baseline:** [`d50af8026938797fc0820d0107f4c4e064c2d2bc`](https://github.com/retsyx/amity-esp32/tree/d50af8026938797fc0820d0107f4c4e064c2d2bc), “Dynamic determination of front side physical address.” The documentation is a proposal against this snapshot. Future implementing agents must inspect upstream changes before applying these findings.

## 1. Source requirements

[Amity issue 1 — The Octopus](https://github.com/retsyx/amity/issues/1) requests additional CEC splices for multiple displays, direct-TV playback devices and suppression of source theft before the switching device reacts. Its secondary-output discussion is an example of receiver behavior, not proof that every receiver has the same CEC wiring. It explicitly does not claim that splicing direct sources enables built-in TV applications.

[Amity issue 2 — The Consortium](https://github.com/retsyx/amity/issues/2) proposes clustered Amities, potentially octopuses, for physically distributed systems such as a living-room TV and remote AV rack. Neither issue specifies a wire protocol, security authority or election algorithm. The coordinator, transport, ID and enrollment choices in this package are proposed design decisions.

The current user additionally requested up to five pass-throughs per ESP32, agent-oriented implementation documents, BLE/Matter coexistence, message routing, HDMI scanning and fully qualified names containing octopus/tentacle scope. The user clarified that configured device names must use OSD names, while changing logical addresses are resolved at runtime. The earlier scenario adds TV, projector and shared-receiver routing and easy group addition/removal.

## 2. SDK pins

From the repository's [SDK_VERSIONS.txt](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/SDK_VERSIONS.txt):

| Dependency | Pinned revision |
|---|---|
| ESP-IDF, project 6.1 baseline | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| ESP-Matter | `59574f3fb62fddc690b4127fd3a4a43cfce4245e` |
| ConnectedHomeIP | `539342f32d5f4dc93761c2f9325afe29270068f1` |
| Vendored HomeKit subset | `676fabac4a4a05184be020611cb069faa0016411` |

The implementation must recheck exact pinned headers and behavior when changing these versions. Current online “latest” documentation is supporting context, not a substitute for pinned integration tests.

## 3. Existing implementation facts and implications

| Existing fact | Source | Design implication |
|---|---|---|
| One global HubService with one front/back pair and one selected activity. | [main.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/main/main.cpp), [hub.hpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/include/amity/hub.hpp) | Introduce SystemControl/GroupService and explicit local executors; do not instantiate competing hubs. |
| Activities resolve by OSD name; the resolver already follows a known name moving to a new LA. | [hub.hpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/include/amity/hub.hpp), [hub.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp) | Preserve automatic name resolution, add consortium/octopus/tentacle/side scope, and separate configured names from versioned runtime addresses. |
| Each Bus allocates a GPTimer, worker stack and bounded queues. | [bus.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/cec/bus.cpp) | Ten-line scaling requires shared timing and explicit memory/scheduling work. |
| Engine receive filtering excludes foreign directed messages from delivered frames. | [engine.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/cec/engine.cpp) | Add observe-all mode without expanding ACK ownership. |
| Hub configure can enter standby; discovery/identity can trigger route restoration. | [hub.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp) | Configuration/discovery must no longer independently mutate group desired state. |
| HubService reserves admission before persistence, rejects stale generation work and preserves emergency release. | [hub_service.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub_service.cpp) | Preserve these local invariants while adding network boundaries. |
| Current physical-address startup is dynamic but heuristic. | [hub.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub.cpp), [startup scan](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_hub/hub_service.cpp) | Preserve as scoped evidence, not a global HDMI topology oracle. |
| Matter callbacks provide a command boundary, while web/HomeKit also read single-hub state. | [Matter interface](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_matter/include/amity_matter.h), [web admin](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/web_admin.cpp), [HomeKit](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_homekit/homekit.cpp) | Adapt all ingress/state consumers to SystemControl and explicit zone snapshots. |
| Remote stamps use local time/activity generation/target; one local managed remote. | [remote_input.hpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/ble_remote/include/remote_input.hpp), [ble_remote.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/ble_remote/ble_remote.cpp) | Add origin/RemoteId/session and explicit zone ownership; never compare independent monotonic clocks. |
| Config parsing is strict and size-bounded; activity external identifiers are positional. | [configuration.cpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/web_admin/configuration.cpp), [HomeKit](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/amity_homekit/homekit.cpp) | Keep parser discipline; migrate to stable IDs/slot tombstones before reorder. |
| Factory format v3 is 288 bytes with reserved bytes required zero. | [factory_credentials.hpp](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/components/device_settings/include/factory_credentials.hpp) | New bootstrap identity needs explicit origin/factory versioning, not silent use of reserved space. |
| Project documents no ARC/eARC or direct-TV-source support in its current single-tentacle design. | [README](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/README.md) | Direct-source control is new scope; return audio and TV applications remain separate gates. |
| Flash and existing service budgets are finite. | [partitions.csv](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/partitions.csv), [sdkconfig.defaults](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/sdkconfig.defaults) | Measure combined TLS/CEC/radio/OTA capacity, not isolated feature memory. |

### October 19 physical-address behavior

The reviewed version initializes front PA to unknown, scans the back side, chooses the numerically lowest usable nonzero back-side report, or uses configured fallback. It claims/announces the front identity after that decision and holds the result for the boot. Later scans do not automatically revise it. This is newer than the earlier `ca36720` review and supersedes any claim that the firmware simply uses a fixed front PA at startup.

It does not read EDID or prove the report came from the immediate peer. Full source isolation can remove native reports from a shared receiver domain, and a receiver's two output paths may need different evidence. Those are design gaps addressed by [document 05](05-discovery-and-topology.md). [Relevant tests](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_front_physical_test.cpp).

## 4. External primary sources

| Source | Relevant evidence |
|---|---|
| [Espressif ESP32-S3 datasheet v2.2](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) | Four general-purpose timers, GPIO restrictions, memory/radio resources. |
| [ESP-IDF tagged S3 SoC capability header](https://github.com/espressif/esp-idf/blob/v5.5.5/components/soc/esp32s3/include/soc/soc_caps.h) | Supporting chip resource count; not the project's SDK pin. |
| [ESP-IDF GPTimer documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/gptimer.html) | Timer callbacks, resource allocation and cache/IRAM requirements. |
| [Unified provisioning](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/provisioning/provisioning.html), [protocomm](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/provisioning/protocomm.html) | Security2 SRP/channel framework and transports; exact client/host integration still requires work. |
| [Pinned ESP-Matter controller Kconfig](https://github.com/espressif/esp-matter/blob/59574f3fb62fddc690b4127fd3a4a43cfce4245e/components/esp_matter_controller/Kconfig) | Stock commissioner configuration excludes simultaneous Matter server path. Do not remove the guard as a design solution. |
| [Pinned CHIP NimBLE implementation](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/platform/ESP32/nimble/BLEManagerImpl.cpp) | Pre-start extra GATT service hook, shared-host and advertising behavior. |
| [Pinned CHIP Wi-Fi driver](https://github.com/project-chip/connectedhomeip/blob/539342f32d5f4dc93761c2f9325afe29270068f1/src/platform/ESP32/NetworkCommissioningDriver.cpp) | Staged network commit/rollback integration; exact use is described in document 10. |
| [ESP-TLS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_tls.html), [ESP-IDF security](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/security.html) | Verification and production protection mechanisms; not evidence that Amity enables them. |
| [Pinned Espressif Mbed TLS header](https://github.com/espressif/mbedtls/blob/a2b32072ea898afc1ed5b6caf6931e36028c91d6/include/mbedtls/ssl.h) | Per-configuration verification API underlying the proposed operational pin verifier. |
| [Linux CEC physical-address API](https://docs.kernel.org/userspace-api/media/cec/cec-ioc-adap-g-phys-addr.html) | Physical-address/EDID relationship and unknown-address handling as implementation context. |

Detailed implementation guides cite the specific source paths used for their claims, including proof-format differences between HomeKit SRP and protocomm Security2. The proposal does not reproduce or purport to replace the HDMI-CEC specification; opcode implementations must be checked against the supported CEC version and licensed/available normative references during implementation.

## 5. Existing test assets

The repository has useful starting points: [host test runner](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/run_host_tests.sh), [hub commit tests](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_hub_commit_test.cpp), [remote input tests](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/remote_input_test.cpp), [factory reset tests](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/factory_reset_test.cpp), [physical-address hardware test](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_front_physical_hardware.py) and [CEC/web activity test](https://github.com/retsyx/amity-esp32/blob/d50af8026938797fc0820d0107f4c4e064c2d2bc/tests/cec_web_activity.py).

Their current single-node or two-interface fixtures do not establish ten-line timing, distributed authority or prevention before source theft reaches the receiver. Mocked browser tests do not prove actual CEC behavior. [Document 15](15-validation-and-release-gates.md) specifies extensions and evidence classes.

## 6. Review limits

This was source and documentation analysis. No firmware implementation, build, flashing, CEC capture, BLE pairing, Matter commissioning, physical pin measurement, runtime capacity benchmark or security certification was performed. The existing source repository was not modified or published. Proposed numbers such as eight members, 8192-byte frames and held-key lease periods are explicit initial design/qualification targets, not measured capabilities of the current firmware.
