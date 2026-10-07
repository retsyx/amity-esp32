# Validation record

Target: Seeed XIAO ESP32-S3, 8 MB flash and 8 MB octal PSRAM; installed ESP-IDF 6.1 / ESP-Matter commits in [SDK_VERSIONS.txt](SDK_VERSIONS.txt). Hardware fixture: Raspberry Pi `test2`, two isolated Linux CEC GPIO adapters and a BlueZ HID keyboard emulator.

Current build: **0.1.46** with Siri Remote support, Matter software version 47, 1,862,224-byte application, installed on `amity-dut` through HTTPS OTA. Each OTA slot is 3.8125 MiB. Previous firmware has been installed through the actual HTTPS OTA endpoint, including version upgrades 0.1.3 → 0.1.4 → 0.1.5 → 0.1.6. Version 0.1.7 was installed over USB while preserving NVS; 0.1.8 through 0.1.23 were installed through HTTPS OTA and retain the user-set password and saved device state.

## Siri Remote Gen 2/3 support

The firmware builds with Apple's firmware-specific setup, Gen 2/3 buttons and
touch gestures, while retaining the single saved remote and existing Remote tab.
The 1,862,224-byte application fits the existing OTA slots with 53% free. The
complete host suite passes with ASan/UBSan; Siri tests additionally verify model
identification, older-model exclusion, both firmware layouts, all buttons,
standby activity selection, long presses, power/activity combinations, touch
pressure and packed coordinates, axis locking, timestamp wrap, release inertia,
multi-touch cancellation, contact-ID changes after lost releases, and finite
swipe pacing. Real hub-worker tests verify generation-bound held presses cannot
cross an activity change. Existing Atomic tests pass and web assets are unchanged.

Evidence: `test-results/siri-build.log`, `test-results/siri-host-tests.log` and
`test-results/siri-ble-tests.log`.
HTTPS OTA on 2026-10-07 installed the verified Siri-support image on `amity-dut`.
The prior firmware connected the Siri Remote through its generic HID adapter.
The new boot identifies the existing remote as Gen 2 (product `0x0314`, firmware
`0x0083`), completes Apple's notification setup, reconnects with the saved bond
and reports 74% battery. All twelve deployment checks passed, retaining saved
activities, Matter fabrics, HomeKit controllers, hostname, CEC address, password
and TLS identity, and restoring the original active activity. Fresh pairing,
Gen 3 hardware and physical button/touch gestures remain unverified.
Evidence: `test-results/siri-power-build.log`, `siri-power-ble-tests.log`,
`siri-power-deployment.json` and private `siri-power-post-ota.log` under
`test-results/`.

## Matching Matter and HomeKit wording in 0.1.46

Both tabs use shared status wording for connected, disconnected, ready to pair,
busy and failed operations. Pairing instructions, Apple Home removal labels and
confirmations also match. Matter operations now display the same progress message
as HomeKit. The HomeKit Wi-Fi prerequisite remains visible when unavailable.
Firmware and provisioning package builds, eight HomeKit lifecycle checks and 81
activity/Matter UI checks passed. HTTPS OTA on `amity-dut` preserved configuration,
Matter fabrics, remote bond, HomeKit pairing, password, hostname, CEC address and
TLS identity. The live browser verified matching messages across equivalent
states and exact current UI assets without changing pairing state.
Evidence: `test-results/pairing-copy-build.log`, `pairing-copy-homekit-ui.log`,
`pairing-copy-activities-ui.log`, `pairing-copy-deployment.json` and
`pairing-copy-live.json`, all under `test-results/`.

## HomeKit tab copy in 0.1.45

Removed the introductory television and Matter commissioning paragraph. Firmware
and provisioning package builds passed. HTTPS OTA on `amity-dut` preserved its
configuration, Matter fabrics, remote bond, HomeKit pairing, password, hostname,
CEC address and TLS identity. A live browser confirmed the paragraph is absent,
the HomeKit tab loads without script errors, and all served UI assets match the
current source. Evidence: `test-results/homekit-copy-build.log`,
`test-results/homekit-copy-deployment.json` and `test-results/homekit-copy-live.json`.

## Optional HomeKit television in 0.1.44

Removed Matter endpoint 4 and its creation, handlers, attribute reporting and
controller test options. Matter root PartsList now contains only endpoints 1,
2 and 3. Added independent HomeKit Television, linked Television Speaker,
16 stable activity Input Source slots and remote Battery services. Unused slots
are unconfigured and hidden. HomeKit controls use the existing CEC hub and its
settling guard; speaker volume is relative and unknown readings report resource
absent. Names and input visibility preferences persist across reboot.

Matter supplies Wi-Fi first. HomeKit pairing is optional and is enabled only
through a separate authenticated web tab. The connected Wi-Fi station determines
availability, including after Matter fabrics are removed. Each pairing request
generates a fresh PIN/verifier and QR; setup material is hidden outside the
five-minute window. Only the salt/verifier and setup ID persist, and the code is
excluded from logs and provisioning packages. Closed or replaced windows reject
incomplete setup sessions. HomeKit removal preserves Matter, Wi-Fi and BLE state.

The selected Espressif HAP components are vendored at the commit recorded in
SDK_VERSIONS.txt, with local IDF 6.1 crypto, transport, memory and shared mDNS
adaptations. Installed ESP-IDF and ESP-Matter SDK sources are unchanged. HAP
allocations and the 24 KiB diagnostic ring use PSRAM; stacks remain internal.
The final 1,852,272-byte application fits each 3,997,696-byte OTA slot. A real
simultaneous HomeKit encrypted session, Matter CASE read, bonded BLE keyboard
and HTTPS request passed with 90,679 bytes of internal heap free and a 43,008-byte
largest internal block. This is a measured test state, not a worst-case bound.

Host sanitizer tests, eight HomeKit browser-fixture checks, all 81 activity checks,
15 firmware-update UI checks, 12 device-settings checks and provisioning Go tests
passed. Firmware/package builds and package hashes passed. The isolated
`aiohomekit==3.2.20` controller verified SRP, wrong-PIN rejection, window cancellation,
encrypted reads/writes/events, complete linked services, observed controls,
battery data, controller removal and reboot persistence. Physical CEC captures
confirmed navigation, play, pause and television-side relative volume. The live browser QR was independently decoded with macOS Vision; the actual
five-minute timeout removed backend and browser setup material. Native Apple
Home UI presentation still requires a check with Apple's Home app.

HTTPS OTA installed the final build on amity-dut and retained its settings,
password, hostname, front CEC address and TLS identity. A temporary Matter fabric
and BLE keyboard were used for testing; cleanup restores the original fabric
and remote state. amity-test was not touched. The final OTA preservation audit
ran concurrently with deliberate removal of the temporary fabric, so its
fabric equality assertion is superseded by the final restoration check.
Evidence is in test-results/homekit-dual-build-final.log, homekit-final-hardware.json,
homekit-dual-coexistence.json, web-homekit-live.json and the final restoration
report (`homekit-final-restoration.json`). Pairing screenshots and controller keys remain in private ignored files.

## Device category dropdowns in 0.1.43

Activity and recommendation editors now use separate Display, Source, Audio and
Switch suggestion lists. Scanned CEC primary types determine eligible roles:
TVs support Display and Audio; playback, recording and tuner devices support
Source; audio systems support Audio and Switch; HDMI switches and video
processors support Switch. The type values match the [Linux CEC definitions](https://kernel.org/doc/html/latest/userspace-api/media/cec/cec-ioc-adap-g-log-addrs.html).
Saved names with unavailable CEC types appear only in the role where they are
configured; a known scanned type takes precedence over an incompatible saved
role. Unknown scanned devices without saved roles are omitted. Suggestions
remain sorted and deduplicated, and manual entry remains available.

All 81 activity browser checks passed, covering the new role mapping, playback
exclusion from Display despite a mismatched saved configuration, receivers as
input switches, TV audio, unknown types, recommendation/create/edit dialogs,
offline names, successful and failed saves, discard and reload. Firmware and
provisioning package built successfully; JavaScript syntax and whitespace checks
passed.

HTTPS OTA installed 0.1.43 on `amity-dut` while preserving configuration, password,
hostname, front CEC physical address, TLS identity, Matter fabrics and remote
bond state. The Pi fixture initially had both CEC adapters cleared; restored the test TV
and playback roles after detecting bus identities and verifying CEC responses.
Six live browser checks verified a real scan finding a TV and a
playback device, the filtered lists and dialog bindings, unchanged saved
activities, no script errors and exact current HTML/JavaScript/CSS assets.
`amity-test` was not changed. Evidence: `test-results/device-role-dropdowns-browser.log`,
`device-role-dropdowns-build.log`, `device-role-dropdowns-deployment.json` and
`device-role-dropdowns-live.json` and `device-role-dropdowns-harness.json`.

## Saved activity device suggestions in 0.1.42

Create and Edit activity dialogs share suggestions combining the current HDMI
scan with display, source, audio and optional switch device names from every
saved activity. Suggestions are sorted and deduplicated; blank names are omitted.
Names remain available when scans find no devices. Successful saves update the
suggestions immediately, while staged changes and failed saves retain the saved
baseline. Device names keep their exact original spelling for CEC references.

All 77 activity browser checks passed, including five new checks covering both
dialogs, offline devices, all saved roles, duplicates, empty scans, failed saves,
saved additions/deletions, discard and reload. Firmware and provisioning package
built successfully; JavaScript syntax and whitespace checks passed.

HTTPS OTA to `amity-dut` preserved configuration, password, hostname, front CEC
physical address, TLS identity, Matter fabrics and remote bond state. Six live
browser checks verified the combined suggestions in both dialogs, the installed
version, unchanged saved activities and exact current HTML/JavaScript/CSS assets.
`amity-test` was not changed. Evidence: `test-results/saved-device-names-browser.log`,
`saved-device-names-build.log`, `saved-device-names-deployment.json` and
`saved-device-names-live.json`.

## Firmware update waiting and fresh UI in 0.1.41

The updater replaces the immediate login redirect with upload progress and a
restart/reconnection screen. Public `/api/setup` adds version, a per-boot nonce
and readiness set only after CEC, configuration, Matter, BLE and HTTPS start and
OTA confirmation succeeds. Completion requires a new boot with the requested
version and readiness; upload acknowledgement and an old same-version boot are
insufficient. The browser clears the expired session, suppresses background
login redirects during the update, and retains the pending update in session
storage so a page reload resumes waiting without storing a password.

Polling uses bounded requests and a two-minute deadline with reconnect retry.
Interrupted upload responses are checked against actual boot health. Rollback
reports the running version without claiming success; older firmware lacking
metadata offers login to verify the version. Verified completion reloads the
page, revalidates all no-cache assets, and shows the installed version on the
fresh login page. The new flow takes effect once the 0.1.41 UI is loaded.

All 99 local browser checks passed: 15 firmware-update checks, 72 activity/remote
checks and 12 device-settings checks. Tests cover old boot/same-version safety,
offline and starting states, reload recovery, lost upload response, fresh asset
execution, completion notice, rollback, timeout retry and older metadata APIs.
Firmware and provisioning package built; all image checksums matched.

Eight live browser/OTA checks on `amity-dut` passed. Reinstalling 0.1.41 showed
upload/restart progress while hiding login, verified a changed boot ID and
healthy version, downloaded all three assets with content hashes matching the
new build, and accepted the unchanged admin password afterward. Config,
hostname, TLS identity, Matter fabrics and remote bond state were retained.
`amity-test` was not changed. Actual rollback recovery was simulated in browser
tests; the live test exercised successful OTA and reboot.
Evidence: `test-results/ota-feedback-final-build.log`,
`ota-feedback-browser.log`, `ota-feedback-activities.log`,
`ota-feedback-settings.log`, `ota-feedback-deployment.json`,
`ota-feedback-hardware.log` and `ota-feedback-hardware.json`.

## Weak remote proximity prompt in 0.1.40

During automatic pairing, a weak HID advertisement now prompts:
“Remote detected. Bring it closer to Amity.” The API derives `too_far` from
scan candidates below the configured threshold, including advertisements seen
before the remote allows connections. Rejected candidates are excluded, and
a qualifying connectable candidate, bonding, or the end of search clears the
hint. Pairing eligibility and the −50 dBm threshold are unchanged. The live
status region stays outside `aria-busy` so the prompt can be announced while
the button remains busy and the indeterminate progress indicator stays visible.

All 72 browser checks passed, including showing the weak-signal prompt without
stopping search, clearing it for a stronger signal, and pairing/timeout states.
Firmware and provisioning package built successfully; image checksums matched.
HTTPS OTA on `amity-dut` retained configuration, hostname, CEC address, password,
TLS identity, Matter fabrics and remote bond state. Seven live browser/API checks
confirmed a real weak Couch Clicker triggers the prompt during scanning, keeps
progress visible, and returns to timeout feedback without creating a bond or
changing config/fabrics. `amity-test` was not changed.
Evidence: `test-results/remote-proximity-final-build.log`,
`remote-proximity-browser-tests.log`, `remote-proximity-deployment.json`,
`remote-proximity-live.json` and `remote-proximity-live.png`.

## Automatic nearby remote pairing in 0.1.39

Pair Remote starts a 20-second automatic search. The first connectable HID peer
at or above −50 dBm is selected, matching `amity/pair_tool.py`; the build setting
`CONFIG_AMITY_REMOTE_PAIRING_MIN_RSSI` controls the threshold. Rejected peers are
skipped for the rest of that search, and security/discovery runs on the worker,
not the NimBLE scan callback. Bond acceptance is verified through encryption and
HID discovery, since advertisements do not reliably identify pairing mode.
Security errors survive the cleanup disconnect for useful failure reporting.
The UI removes the candidate list and shows immediate searching/pairing text,
an indeterminate progress indicator, and timeout/failure instructions.

All sanitized host suites and 70 browser checks passed, including the RSSI
boundary, non-HID/nonconnectable/rejected candidates, pending submission,
search/connection progress, retry feedback and completion. Firmware and package
built successfully; all package image checksums matched. HTTPS OTA on
`amity-dut` preserved config, password, TLS identity, hostname, CEC address,
Matter fabrics and the remote bond state.

Nine real browser/radio checks on `amity-dut` passed using the Pi HID fixture:
empty search timeout, skipping a peer rejecting new bonds, first-nearby automatic
bonding, encrypted HID battery reads and UI completion without a device list.
A distant Couch Clicker was ignored while the stronger fixture was selected.
Temporary bonds were removed, the original Pi Pairable setting restored, and
DUT config/fabrics retained. `amity-test` was not used for these tests.
Evidence: `test-results/auto-remote-build.log`, `auto-remote-host-tests.log`,
`auto-remote-browser-tests.log`, `auto-remote-deployment.json`,
`auto-remote-hardware.log` and `ble-auto-pair-hardware.json`.

## Apple Keychain visibility and cleanup in 0.1.38

Apple Keychain (vendor 0x1384) is always hidden in the admin UI and does not count
as a visible connection. The UI requests removal of the selected Home only.
A firmware FabricTable delegate handles removals from any controller or the
admin API: after the last committed Apple Home (0x1349) is removed, it schedules
deletion of all remaining Keychain fabrics. Another Home retains its Keychain
credentials; unrelated platforms are preserved. Incomplete pairing rollback
does not trigger cleanup. A persisted intent marker supports recovery if a
reboot interrupts cleanup; work waits for the commissioning failsafe to disarm.
This uses known vendor IDs; fabric metadata has no permanent parent relationship.
Keychain-only pairing without a Home removal is hidden and retained.

All sanitized host suites and 67 local browser checks passed. New policy tests
cover single/multiple Homes, failed uncommitted pairing, pending cleanup recovery,
new Home arrival before cleanup, index reuse and invalid/duplicate notifications.
Firmware and provisioning package built successfully and image checksums passed.
HTTPS OTA retained configuration, hostname, front physical address, password,
TLS identity, remote bond and fabrics. A live browser confirmed the actual
orphan was hidden and the UI showed Not connected before it was cleaned up.

Real IP/CASE tests created temporary observer, Keychain and two Home fabrics,
using the known Apple vendor IDs without Apple services. Operational Credentials
RemoveFabric preserved the second Home and both Keychains after the first Home
was removed. Removing the last Home automatically deleted the test Keychain and
the user-reported pre-existing orphan, while the observer stayed operational.
All test fabrics were removed; configuration, bond, activity and the initial
commissioning-window state were preserved. Actual removal from Apple's Home app
and power-loss recovery of the marker have not been independently exercised.
Evidence: `test-results/build-keychain-cleanup.log`, `host-keychain-cleanup.log`,
`web-keychain-cleanup.log`, `keychain-cleanup-deployment.json`,
`apple-cleanup-live-ui.json` and `apple-cleanup-hardware-result.json`.

## Matter on/off compatibility endpoint in 0.1.37

Endpoint 4 is an On/Off Plug-in Unit (0x010A), appended alongside the existing
player, speaker and remote battery. On, Off and Toggle use the player's existing
power callback and observed hub state. Both power endpoints report activity
start/stop changes. Endpoint capacity increases from four to five including root.
The SDK supplies Descriptor, Identify, Groups, On/Off and Scenes Management.
Only ordinary On/Off/Toggle commands are wired to CEC hub power; additional
lighting and scene behavior remains outside this experiment.

Firmware and provisioning package built successfully, with image hashes and
the embedded version verified. HTTPS OTA from 0.1.35 retained configuration,
hostname, front physical address, password, TLS identity, remote bond and fabrics.
An actual Matter IP PASE session verified endpoint type, all five server clusters,
root PartsList membership and shared player power. Endpoint 4 On, Off and both
Toggle directions passed, and endpoint 1 On/Off updated endpoint 4's state.
The initial standby state was restored; no test fabric was installed and no WiFi
configuration was changed. The user confirmed Apple Home presents Amity as an
outlet. Subscription delivery has not been independently tested. Evidence:
`test-results/build-power-switch.log`, `power-switch-deployment.json` and
`matter-power-switch-result.json`.

## Matter diagnostics in 0.1.36

Diagnostic Logs is enabled on endpoint 0 with ResponsePayload and BDX retrieval
of the existing bounded runtime log ring. EndUserSupport and NetworkDiag are
supported; CrashLogs reports NoLogs because crash dumps are not retained.
General Diagnostics adds BootReason and persistent TotalOperationalHours, plus
the standard fault events. Wi-Fi Diagnostics adds the SDK's connection events;
packet/error count features remain disabled as requested, without SDK changes.

All sanitized host suites passed. New tests cover independent log snapshots,
chunk reconstruction, response truncation, cancellation, bounded sessions,
invalid/stale handles, handle wrap, fractional-hour accumulation across reboots,
and counter saturation. Firmware and provisioning package built successfully;
package image checksums and the embedded version were verified. Live Matter log
retrieval, BDX transfers and persistence on the DUT have not been tested.
Evidence: `test-results/build-diagnostics.log` and `host-diagnostics.log`.

## Standalone repository migration

On 2026-09-30, the complete former `amity/esp32` directory moved to
`~/projects-other/amity-esp32`. Local build/test instructions and default output
paths now start at this repository root. Provisioning release archives take
`LICENSE` from this repository. The existing Raspberry Pi installation remains
at `/home/pi/amity/esp32`.

Both firmware projects were rebuilt from fresh build directories: the main
application plus `provisioning-package`, and `tests/cec_probe`. All host suites,
Go tests/vet, factory-record/reset tests, and 63 browser checks passed. All six
Mac/Windows/Linux × AMD64/ARM64 provisioning executables and release archives
were rebuilt; archive checksums and bundled licenses were checked. The native
Mac ARM64 executable successfully provisioned the new firmware ZIP, including a
custom hostname, generated initial password, 8 MB image and QR output. Cross-built
Windows and Linux executables were not run in this migration pass.

Private test data and file permissions were preserved. Prior build trees remain
under ignored `.cache/pre-move/`; active build metadata points only to the new
project location. Evidence is in `test-results/build-standalone.log`,
`build-standalone-cec-probe.log`, `standalone-host-release.log`, the browser JSON
reports, and the private `standalone-provision-smoke/` directory.

## Passed host checks

ASan/UBSan suites cover generic CEC encoding/decoding, ACK/arbitration/deadlines, activity routing and power behavior, identity replies, device rediscovery, command reservation and commit ordering, full queues, emergency releases, activity-generation cancellation, HID descriptors, Atomic mappings/holds/repeats, and configuration schema/bounds. Browser JavaScript syntax also passes.

## Provisioning tool checks

The provisioning PNG contains a 512-pixel QR code with its original quiet zone,
a dashed manual code below it, and hostname/admin URL. It includes the initial
password only for `--generate-password`; user-supplied and prompted passwords
are omitted from both the image and pairing text, and are stored only as hashes
in factory data. With no password configured, the card says “Not set.” Go tests
verify QR pixels, generated credentials and omission of supplied passwords.
Secure terminal tests cover hidden entry, omitted output, mismatch and cancellation.
All six release targets build with the embedded Go font license. This change
requires no firmware update.

`sh tests/provisioning_test.sh` passes Go tests/vet and the ASan/UBSan C++
factory-record parser tests. Coverage includes Matter setup-code vectors, invalid
PINs, random PIN generation, a shared Go/Python/C++ format fixture, damaged or
unsupported records, incompatible/missing/duplicate/oversized package images,
partition flags/checksum/layout, full-image offsets, and refusal to overwrite
an existing output directory.

`idf.py build provisioning-package` succeeds. The resulting firmware ZIP was
provisioned by standalone Mac ARM64 and Linux ARM64 executables. All six
Darwin/Windows/Linux × AMD64/ARM64 executables cross-compile with CGO disabled;
Windows and the remaining four host combinations were not run natively.
macOS Vision independently decoded the generated PNG to the expected payload.
The official CHIP SDK on the Pi parsed both codes from a randomly provisioned
Linux output and independently generated byte-for-byte identical codes.

The full fresh-install image was checked for correct assembly; it was not flashed
over the user's configured DUT. Hardware validation instead installed its exact
factory sector and matching partition table after a full 8 MB flash backup,
leaving every existing partition's address and contents intact. No destructive
factory reset or fresh Apple Home commissioning was performed in this pass.

## Hardware evidence

* **0.1.35 Advanced section order:** all seven cards now appear as Configuration, Diagnostics, Update Firmware, Hostname, HDMI-CEC, Password, Factory Reset. The 12 existing device-settings and six firmware-upload browser checks passed. Firmware and provisioning package built successfully. HTTPS OTA and a read-only live browser check confirmed the requested order on the DUT; password, hostname, saved configuration, TLS identity, Matter fabrics, remote bond and activity were retained. Evidence: `test-results/build-advanced-order.log`, `web-device-settings-ui.json`, `web-firmware-ui.json`, and `advanced-order-live.json`/`.png`.

* **0.1.34 activity settling guard:** added `CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS` under Amity activity control (default 5; 0 disables it). Admission reserves the target before enqueue and rejects a different activity/standby request during setup and settling; ignored requests are never deferred. Same-target Repair, including standby repair, remains available and does not restart the period. Remote Power uses the reserved target so rapid presses during startup cannot be misclassified as repairs. Remote, web and Matter share the guard; web activity rejection reports HTTP 409 with a readable settling message. Setup waits for FIFO transmission completion, tracks matching expected responses from participating devices on the correct bus, and starts quiet time after the final transmission/expected reply. Missing replies time out after two seconds; malformed, unrelated, duplicate and subsequent repair traffic does not extend the guard. Configuration replacement clears obsolete activity reservations. All sanitized host suites passed, including independent exact-boundary/response/timeout/custom-delay/disabled tests and real hub admission, pre-execution rejection, expected-response delivery, repair, standby, queue saturation and failed-admission checks. All 65 activity browser checks passed. Firmware and provisioning package built with the default value 5. HTTPS OTA retained configuration, password, hostname, TLS identity, Matter fabrics, remote bond and activity. Live DUT tests injected a delayed Active Source response from HarnessSource: a different activity request remained rejected until approximately 5.16 seconds after the reply. Standby settling measured approximately 4.93 seconds from observing the completed transition; same-target repair during both periods remained accepted without restarting the timer. Rejected startup/standby requests did not run after settling. Original configuration and activity were restored. Evidence: `test-results/host-activity-settling.log`, `web-activity-settling.log`, `build-activity-settling.log`, `activity-settling-live.json`, `activity-settling-http.json`, and private pre/post-update snapshots.

* **0.1.33 completion-driven held keys:** removed the Atomic controller's 120 ms repeat timer. The hub queues one held press through the ordinary CEC FIFO, waits for its wire attempt to complete, then checks pending commands and held state before immediately sending another. Release advances a cancellation generation even while transmission/discovery is in progress; a quick tap still sends one press/release, and activity/configuration changes stop continuation. Normal one-shot commands retain their press/release behavior. CEC framing, arbitration and idle timing are unchanged. All ASan/UBSan host suites passed, including new deterministic checks for no fixed repeat wait, release during the first or later transmission, release before owner execution, one-shot behavior and activity changes during transmission; existing queue-overflow release recovery and Atomic alias/long-gesture/cancellation checks also passed. Firmware and provisioning package built successfully. HTTPS OTA retained configuration, password, TLS identity, hostname, Matter fabrics, remote bond and activity. With the user's paired Atomic, two actual Volume Up holds produced 50 and 39 CEC press frames followed by a release in each case. Within each hold, receive timestamps were 92.400–92.501 ms apart (92.450 ms median), matching frame duration plus the CEC bus gap, with no added transmit errors. The release frame completed approximately 68.45 ms after the final press frame; this is wire-frame timestamp spacing, not a measurement of physical key-up latency. No repeat backlog followed the release. Original activity/configuration were restored, and the Atomic remained connected. Evidence: `test-results/host-cec-repeat.log`, `build-cec-repeat.log`, `cec-repeat-hardware.json`, `cec-repeat-cec0.log`, `cec-repeat-cec1.log`, and private capture/pre/post-update snapshots.

* **0.1.32 discard activities and harness discovery:** added Discard changes alongside Save activities, visible only for unsaved changes and locked during scanning/saving/activity commands. A full saved snapshot restores edited/deleted/added activities and their order without any configuration write; successful saves replace the snapshot, and discarding restores the correct Start/Repair mapping. All 65 activity browser checks passed, including discard after save failure and repeated edits after discard. Firmware and provisioning package built successfully; JavaScript syntax and whitespace checks passed. Empty HDMI scans were caused by the previous physical-address test clearing both Pi CEC adapters, leaving no simulated devices to discover. Reconfigured and checked both buses with `tests/cec_hardware.py`; automatic bus detection found front `/dev/cec0` and back `/dev/cec1`. A real DUT scan then found HarnessTV and HarnessSource while preserving saved activities. Both adapters remain configured for further development. HTTPS OTA retained password, hostname, configuration, TLS identity, Matter fabrics, remote bond and activity. Live Chromium verified real discovery while unsaved edits were pending, scan-time Discard locking, full restoration without any configuration POST, and fresh discovery remaining visible after discard. The DUT served the exact tested assets, and saved configuration/current activity were unchanged. Evidence: `test-results/web-discard-scan.log`, `discard-scan-harness.json`, `discard-scan-discovery.json`, `discard-scan-live.json`, `discard-scan-live.png`, `build-discard-scan.log`, and private pre/post-update snapshots.

* **0.1.31 activity editing dialog:** the main list contains activity names, Edit/Delete/Start or Repair controls, and drag handles. Add and Edit open a responsive modal with isolated drafts, OK/Cancel, and Start/Repair; changed settings offer Save and start/repair with an explicit notice that all pending activity changes are saved. Saved activity identity is retained through unsaved edits, deletions and reordering. Save failures preserve drafts and never start stale settings; a successful save immediately updates the displayed baseline. Preflight configuration reads detect changes already saved by another session before starting or overwriting activities. These checks are not atomic across concurrent requests from different clients. All 62 activity browser checks passed, including real mouse/touch dragging, keyboard reordering, modal cancellation, saved-index targeting, failed and pending saves, cross-session changes, scan locking and session expiry. The 12 Advanced settings and 6 firmware-upload browser checks also passed, along with JavaScript syntax and whitespace checks. Firmware and provisioning package built successfully. HTTPS OTA retained password, hostname, configuration, TLS identity, Matter fabrics, remote bond and activity. Live Chromium verified compact rows, edit/add cancellation, drag ordering, saved-index Start/Repair from both list and dialog, and reload restoring persisted order. The original configuration and activity were retained. Desktop/mobile screenshots were inspected. Evidence: `test-results/web-activity-modal.log`, `activity-modal-live.json`, `activity-modal-live.png`, `activity-modal-mobile.png`, `build-activity-modal.log`, and private pre/post-update snapshots.

* **0.1.30 remote battery and charging:** enabled the Matter Power Source cluster and four total endpoints; endpoint 3 is "Remote battery" with Battery/Rechargeable features. The BLE client reads/subscribes to Battery Level, Battery Level Status (including indications), and legacy Battery Power State, with polling for readable characteristics lacking subscriptions. Malformed, missing, absent, and unknown values are covered by sanitized parser tests. The public status policy is explicitly tested for unpaired defaults (100%/not charging), disconnect (both unknown), reconnect without readings, and unpair with stale readings. All host suites and 51 browser checks passed; Diagnostics only adds "Charging" for an explicitly charging connected remote. Firmware and provisioning package built successfully. HTTPS OTA retained user configuration, password, TLS identity, Matter fabrics, remote bond, hostname and activity. The Atomic reconnected and reported 60%; the Pi read actual Matter `BatPercentRemaining=120`, `BatChargeState=Unknown`, feature map 6, and description "Remote battery" over BLE PASE without adding a fabric. Live Chromium verified Diagnostics matches the reported state. The installed Atomic firmware exposed Battery Level (`0x2A19`) but neither monitored charging-status characteristic, so a physical charging/not-charging transition could not be verified with this remote; its charging state remains unknown. Charging transitions are covered by parser/UI tests, and the BLE emulator now supports level/status notifications and status indications for future hardware tests. Evidence: `test-results/host-remote-battery.log`, `web-remote-battery.log`, `battery-live-ui.json`, `matter-battery-result.json`, `build-remote-battery.log`, and private pre/post-update snapshots.

* **0.1.29 front CEC physical address:** added the Advanced editor with persistent dotted hexadecimal addresses; blank restores `1.0.0.0`, and saving reboots. Host tests exhaustively validate all 65,536 address values and malformed input. All host/provisioning suites passed, including reset recovery across 59 simulated power interruptions with the new NVS key. Twelve device-settings browser checks passed, covering edited/empty input during polling, save failures and default restoration. HTTPS OTA preserved existing configuration, password, TLS identity, Matter fabrics and the remote bond. Live Chromium saved `2.0.0.0`, then an empty value; the Pi verified actual front-bus Report Physical Address responses of `2.0.0.0` and `1.0.0.0` after their respective reboots, with the back bus unchanged at `0.0.0.0`. Live API probes rejected invalid types/topology and confirmed activity backup/restore excludes and preserves the device address. DUT address, activity and harness adapters were restored afterward. Evidence: `test-results/front-physical-hardware.json`, `front-physical-cec-commands.json`, `web-device-settings-ui.json`, `host-front-physical.log`, and `provisioning-front-physical.log`.

* **0.1.28 factory reset wording:** changed the confirmation to “This will factory reset Amity. You will need to reconfigure it again. Are you sure?” All seven existing device-settings browser checks passed. HTTPS OTA retained the password, hostname, TLS identity, configuration, Matter fabrics, remote bond and activity. Live Chromium verified the exact text and cancelled without sending a reset request.

* **0.1.27 firmware version confirmation:** the browser reads the embedded application version before uploading and warns that older firmware is unsupported and at the user's own risk. Unknown version formats also require confirmation. The binary upload body is unchanged. The device checks semantic-version precedence before beginning OTA and requires an explicit approval header tied to its running version for older or unknown versions.

  All host suites passed, including 27 shared C++/JavaScript version vectors and an additional JavaScript malformed-version case. Six local browser checks covered cancellation without upload, reselecting the same file, confirmation headers, ordinary equal/newer versions, unknown versions, malformed files, and server errors. On the DUT, header-only probes verified HTTP 409 for missing/stale approval, and confirmed that approved older/unknown versions still undergo full image and target validation. No older firmware was installed during these probes. Live Chromium verified cancellation sends no upload, then successfully reinstalled the current release using the original raw binary upload. Both the initial 0.1.26 → 0.1.27 update and browser reinstall retained the password, hostname, TLS identity, configuration, Matter fabrics, remote bond and activity. Evidence: `test-results/host-version-check.log`, `web-firmware-ui.json`, `web-firmware-hardware.json`, and private pre/post-update snapshots.

* **0.1.26 initial password and device settings:** added mutually exclusive command-line, random-generation and secure terminal password options; factory records store only the salted hash. Go/Python/C++ fixtures verify PBKDF2 compatibility and all three record versions. Pseudo-terminal tests verify hidden input, confirmation mismatch, and echo restoration on Ctrl-C. Reset recovery passed 57 injected power interruptions with and without legacy Matter credentials. All host suites, 50 existing browser checks, and seven new Advanced/header/footer checks passed; all six release executables build.

  The DUT was updated through HTTPS OTA and renamed to `amity-dut` using the new authenticated hostname endpoint. Live Chromium verified the hostname header, footer, Advanced editor and cancelled reset confirmation. API tests rejected a reset without confirmation and configuration restores containing hostname or Matter fields.

  After an 8 MB flash backup with a verified SHA-256 digest, a temporary version-3 factory record was installed with a generated initial password and `amity-reset-test` hostname. The existing user password and hostname remained active until an actual API factory reset. Reset then cleared activities, Wi-Fi, original Matter fabrics and remote bonding; it restored the provisioned hostname and seeded the initial password. The Pi commissioned the reset device over BLE/Wi-Fi using the original setup code, and HTTPS login with the generated password succeeded. The new TLS certificate's CN matched the provisioned hostname. The full saved flash was restored afterward to retain the user's original password, TLS identity, configuration, Apple fabrics and Atomic bond, with the requested `amity-dut` runtime name. Private evidence: `factory-reset-hardware-result.json`, `matter-reset-validation-result.json`, terminal/browser test results and reset boot/restore logs under `test-results/`.

* **0.1.25 provisioned hostname:** added `--hostname`, default `amity`, normalized to lowercase and validated against the ESP32 DHCP hostname limit (1–32 letters/digits/hyphens, no leading/trailing hyphen). Go and sanitized C++ tests cover valid and invalid names, omitted defaults, both record versions, checksum coverage and old-package compatibility. All six tool targets build; native Mac ARM64 and Pi Linux ARM64 runs provisioned custom names. The DUT booted with the generated version-2 factory record, `amity-hostname-test.local` resolved to its address, and authenticated HTTPS through that name preserved configuration, both Matter fabrics and the remote bond. The exact original version-1 factory record was restored afterward, returning the device to `amity.local`. New certificates use the provisioned name; existing certificates remain untouched. Evidence is in `test-results/hostname-result.json` and private hostname boot/state logs.

* **0.1.24 offline provisioning:** HTTPS OTA first verified the legacy NVS credential path with the old partition table. The generated `amity_factory` record was then installed in the previously unallocated 4 KB sector at `0xf000`, alongside the new partition table. Startup explicitly confirmed factory credentials were loaded. The running firmware's manual code and QR payload exactly matched the tool's files. A fresh IP PASE session from the Pi authenticated with the matching PIN and read manufacturer `Amity`, model `Amity ESP32`, and version `0.1.24`, adding no fabric. Configuration, password, TLS identity, both original Matter fabrics, the Atomic bond and original activity were preserved. The temporary commissioning window was closed. Private evidence: `test-results/provisioning-factory-boot.log`, `matter-provisioning-result.json`, and pre/post-provisioning snapshots. The DUT remains on 0.1.24 using the provisioned record with its existing pairing code.

* **0.1.23 Advanced tab:** renamed the Settings navigation button to Advanced. Chromium verified the new name, selected state and panel visibility on the DUT. Firmware build and whitespace checks passed; OTA retained configuration, TLS identity, Matter fabrics and the Atomic remote bond, and restored the original activity.

* **0.1.22 Remote pairing status:** all 50 local browser checks passed. Remote displays only Paired or Not paired, based on the saved bond, throughout scanning, connecting, connected, disconnected and error states. Settings Diagnostics shows the live remote connection state, connected name/battery and connection error code. The actual Atomic reports Paired in the DUT Remote tab with connection state in Diagnostics. Firmware build, JavaScript syntax and whitespace checks passed; OTA retained configuration, TLS identity, both Matter fabrics and the Atomic remote bond, and restored the original activity.

* **0.1.21 Matter connection display:** all 48 local browser checks passed. One Apple Home fabric and its Apple Keychain entries are grouped as Apple Home; known platforms use recognizable names and the status reads Connected rather than counting fabrics as homes. Separate Apple homes and standalone Keychain entries stay visible. Fixture checks cover cancelled removal, removing both grouped entries while preserving another platform, failed removal and retry. The live DUT page shows Connected and exactly one Remove Apple Home button while the API retains both original Apple entries. No live fabric removal was performed. Firmware build, JavaScript syntax and whitespace checks passed; OTA retained configuration, TLS identity, Matter fabrics and the Atomic remote bond, and restored the original activity.

* **0.1.20 Matter identity:** configured the SDK Basic Information manufacturer as `Amity` and model as `Amity ESP32` in the project CHIP configuration. A fresh IP PASE session from the Pi read VendorName, ProductName and SoftwareVersionString directly over Matter and verified `Amity`, `Amity ESP32` and `0.1.20`. The probe added no fabric. Firmware build and whitespace checks passed; HTTPS OTA and the probe preserved both existing Matter fabrics, the Atomic remote bond, password, configuration and TLS identity. The temporary pairing window was closed and the original activity restored. Private probe output is in `test-results/matter-identity-result.json`.

* **0.1.19 Remote status:** changed the unpaired status to “Not paired” and verified the actual Remote tab in Chromium. Firmware build, JavaScript syntax and whitespace checks passed; HTTPS OTA preserved password, configuration, TLS identity and Matter/BLE state, and restored the original active activity.

* **0.1.18 Settings byline:** the backup description shares typography and spacing rules with Settings labels. DUT Chromium checks verified matching font size, weight, family, color, line height and vertical margins at desktop and mobile widths, with no horizontal overflow. The mobile screenshot was inspected. Firmware build and whitespace checks passed; HTTPS OTA preserved password, configuration, TLS identity and Matter/BLE state, and restored the original active activity.

* **0.1.17 scan button lock:** all 44 local browser checks passed, including immediate button disabling during scan submission, scans started elsewhere, form edits during scanning, failed scan requests, and restoration with unsaved-edit restrictions preserved. All Activities and recommendation-editor buttons are disabled until the scan completes. A live DUT scan sampled button states 271 times without finding an enabled Activities button, and confirmed controls recover afterward without changing configuration or the active activity. Firmware build, JavaScript syntax and whitespace checks passed; HTTPS OTA preserved password, configuration, TLS identity and Matter/BLE state, and restored the original active activity.

* **0.1.16 activity controls and pairing copy:** all 40 local browser checks passed, including Start/Repair changes as activity state changes, repairing the selected activity, conditional Standby visibility and standby requests, remote button casing and dashed Matter manual codes. DUT Chromium checks exercised Start → Repair → Standby, confirmed the repair request succeeds, checked the remote label and verified the actual Matter code has 4-3-4 digit groups without changing its digits. The original active activity was restored. Firmware build, JavaScript syntax and whitespace checks passed; HTTPS OTA preserved password, configuration, TLS identity, Matter fabrics and BLE bond state.

* **0.1.15 activity equivalence:** all 38 local browser checks passed, including a saved TV-audio activity with a different name and manual switch settings, receiver audio remaining a distinct recommendation, and normal-weight recommendation names. The duplicate on the DUT came from recommending empty audio while the saved activity used the TV; recommendations now use TV audio when no receiver is discovered. `is_equivalent()` compares display/source/audio and ignores name/switch settings, matching the Python management rule. A fresh DUT HDMI scan found both harness devices and omitted the equivalent saved activity. Firmware build, JavaScript syntax and whitespace checks passed; HTTPS OTA preserved password, configuration, TLS identity, Matter fabrics and BLE bond state.

* **0.1.14 Matter status:** zero configured homes now displays “Not connected” when pairing is closed. Chromium verified the device-served renderer against zero, one and two homes and an open pairing window; the DUT automatically opened pairing after reboot, so the zero-home closed-window state was supplied only to the browser renderer. Firmware build, JavaScript syntax and whitespace checks passed. HTTPS OTA preserved password, configuration, TLS identity, Matter fabrics and BLE bond state.

* **0.1.13 header byline:** removed the footer and placed “HDMI-CEC that works!” beneath the Amity title. Desktop and mobile Chromium checks on the DUT verified the byline, absent footer and no horizontal overflow. Firmware build and whitespace checks passed; HTTPS OTA preserved password, configuration, TLS identity, Matter fabrics and BLE bond state.

* **0.1.12 remote toggle:** all 36 local browser checks passed, including the single Pair/Unpair Remote button, discovery and pairing transitions, paired-but-disconnected state, cancelled unpair, failed unpair/retry, and return to Pair Remote after successful removal. All 3 new DUT browser checks passed: the button matches the actual pairing state, starts real BLE discovery when unpaired, disables duplicate scan requests, and becomes available when discovery ends. Existing bonds were left unchanged. Firmware build, JavaScript syntax and whitespace checks passed; OTA preserved password, configuration, TLS identity and Matter/BLE state.

* **0.1.11 device list and status:** all 30 local browser checks and 32 DUT API/browser checks passed. The header stays empty on login, idle/not-ready states and startup failures, and displays only “Scanning HDMI…” during active discovery. Recommendations are omitted from the rendered list when their display/source/audio match an existing activity, including a saved activity with a different name and manual switch configuration. Device rows contain only the sorted OSD name and an accessible inline SVG icon for TV, source, audio, HDMI switch or unknown device. Actual scan results and the rendered device-list screenshot were checked. OTA preserved the user's password, configuration, TLS identity, Matter fabrics and BLE bond state. Firmware build, JavaScript syntax and whitespace checks passed.

* **0.1.10 recommendations and pairing UI:** all 24 local browser checks passed, including recommendation editing, individual append, equivalent-role filtering that ignores names/switch settings, recommendations returning after activity removal, console naming, and the distinction between unconfigured and explicitly empty activity lists. All 8 new DUT browser checks passed for the requested Activities/Settings/Remote changes and the single Matter pairing toggle. macOS Vision independently decoded the browser-rendered QR to the exact Matter payload returned by the device. Closing pairing removes QR and manual-code material; the test restores the original pairing-window state. OTA and UI checks preserved the user's password, activity configuration, TLS identity, Matter fabrics and BLE bond state. Firmware build, JavaScript syntax, Python syntax and whitespace checks passed. QR encoding uses the MIT-licensed Nayuki implementation already bundled in the pinned CHIP SDK.

* **0.1.9 activity UI:** all 29 API/Chromium checks passed on the DUT, including login with Enter, conditional save visibility, reverting edits, real HDMI discovery within Activities, hidden old results after reload, simplified diagnostics, and silent return to login after session expiry. All 15 local browser fixture checks passed, including save failures, edits during a pending save, undoing additions/reordering, optional empty fields, and fresh discovery suggestions. Save visibility compares the current activity values against the successfully loaded/saved configuration; failed saves and edits made during a request retain their unsaved state. A completed-scan counter prevents cached device results being mistaken for a fresh scan. The rendered hardware screenshot was inspected. Host sanitizer suites, JavaScript syntax and whitespace checks passed. OTA preserved the user's password, configuration, TLS identity and Matter/BLE state.

* **0.1.8 login feedback:** all 25 API/Chromium checks passed on the DUT. Login has no greeting, incorrect passwords return HTTP 401 and the red message “The password is incorrect.”, and an active retry cooldown returns HTTP 429 with `Retry-After`. The login button prevents duplicate submissions and respects that delay. Correct login succeeds after repeated failures and cooldown, and the saved user password works after the OTA reboot. The reported rejection was not reproducible with the supplied password before the update, but the old UI hid all login errors behind “Please log in.” Password, configuration, TLS identity and Matter/BLE pairing state were preserved; no credential reset was performed.

* **0.1.7 password length:** all 32 onboarding checks passed on hardware, including a one-character password set through onboarding, a two-character password set through Settings, login with both, and reboot persistence. Empty passwords remain rejected. The 12-character minimum and its UI copy were removed; the 128-byte storage bound remains. JavaScript syntax and whitespace checks passed.

* **0.1.6 UI and onboarding:** the actual HTTPS OTA upgrade preserved the existing password, configuration and TLS certificate. All 18 desktop Chromium checks passed, including removal of the requested copy and all tab headings. All 31 onboarding checks passed on the DUT: locked administrative APIs before setup, invalid-password and content-type rejection, password confirmation, successful setup/login, stale-tab and repeated-setup rejection, session invalidation on reboot, and password/configuration persistence. The mobile onboarding screenshot was inspected. Host sanitizer suites and JavaScript syntax passed again. The generated credential and temporary test password were cleared as requested; the DUT is left at password onboarding, with its existing Wi-Fi, configuration, TLS identity, Matter and BLE state retained. The earlier integration evidence below was collected on 0.1.5.

* **CEC:** 72 exact replies across both physical buses, including 30 simultaneous request pairs, approximately 19 ms latency, and no missing replies or adapter error flags. This also passed during repeated Wi-Fi connection attempts. The HTTPS activity test passed configuration, complete discovery metadata, discovery during an active activity, all eight activities, physical source-address changes and route repair, actual key transmission, power feedback, and enforced standby.
* **Wi-Fi and Matter:** full BLE PASE, attestation, CSR/AddNOC, Wi-Fi provisioning, operational CASE, and CommissioningComplete passed using the Pi controller on the approved home network. Endpoint types 40/34, cluster lists, activities, playback/power/keypad state, unknown audio reporting, and unsupported-command rejection passed. The earlier Wi-Fi failures were traced to the disconnected external antenna; after connection, reception improved to approximately −47 dBm and normal home-network commissioning succeeded.
* **Matter-to-CEC:** 44 checks passed for actual playback, navigation, digits, volume-step and release frames, all input selections, power, observed CEC volume/mute feedback, explicit mute/unmute, and cancellation of pending volume steps on activity change and standby.
* **BLE HID:** encrypted pairing, both report subscriptions, battery level, all eight Atomic activity shortcuts, power behavior, and physical CEC hold/repeat/release frames passed. A subsequent audit found that the shared stack's changing random identity had allowed the emulator to pair again after reboot. Version 0.1.5 uses a stable public identity for HID. All 25 strict checks passed, including reboot with new pairing disabled and the same peer bond identity retained.
* **HTTPS and OTA:** 39 checks passed again on final version 0.1.5: authentication guards, CSP, invalid restores without config changes, nested-input limits, invalid-image rejection, configuration larger than one NVS page, backup/restore, secret exclusion from logs, interrupted valid-image upload, full inactive-slot installation, and persistence of configuration, TLS identity, Matter fabric and BLE bond. Logout revokes the session. The final OTA run also kept new Bluetooth pairing disabled on the peer; afterward the keyboard reconnected encrypted using the same single peer bond identity.
* **Browser:** 11 Chromium checks passed again against final version 0.1.5 after configuration cleanup: login, status, all tabs, unsaved-edit safeguards, session reload, logout, and no script exceptions. The rendered screenshot was inspected. Earlier local API-fixture browser results are separate from these hardware results.
* **Coexistence:** 17 complete HTTPS responses overlapped three fresh operational Matter CASE sessions on final version 0.1.5 with the bonded keyboard connected. Minimum sampled internal free heap was 47,115 bytes. The same test had already passed with 18 responses on 0.1.4. No AES allocation or TLS write failure recurred. TLS buffers and the Matter data model use PSRAM, leaving internal memory for DMA.
* **mDNS:** the DUT responds to direct queries with its current IPv4 address, and operational Matter discovery works. This Mac's system IPv4 resolver returned a different cached/conflicting `amity.local` address; use the DHCP address printed by the DUT if name resolution points elsewhere.

**Management and cleanup:** 18 additional checks passed for post-OTA bond reuse, password rotation, old-session revocation, restoration of the original password, commissioning-window open/close while HID stayed connected, unpairing without fabric loss, and removal of the test fabric without configuration loss. The original activities and home Wi-Fi settings are restored. A final reboot confirms version 0.1.5, no test fabric or keyboard bond, and the original commissioning payload. The temporary Pi AP, USB SSH bridge, watchdog pause and BLE emulator are gone; the original Wi-Fi profile and Bluetooth pairing mode are restored, with no NetworkManager checkpoint or test bond left. Private evidence lives in ignored `test-results/`; it can contain credentials or commissioning material and must not be published.

## Reproduction

Run host checks from the repository root:

```sh
sh tests/run_host_tests.sh
```

Hardware runners under `tests/` require the authorized fixture. Older integration runners read a private legacy boot log for credentials. The browser runner also accepts `--credentials` with a private JSON file containing `admin_password`; new firmware never prints an admin password. The onboarding runner requires a device with no password and creates a temporary test credential that the fixture operator must clear afterward:

| Runner | Purpose |
|---|---|
| `cec_hardware.py` | Assign the two GPIO adapters and validate physical protocol replies |
| `cec_web_activity.py` | Configure eight activities and capture routing/power behavior |
| `ble_hid_emulator.py` + `ble_hardware.py` | Real HOGP pairing, key/CEC behavior, strict reboot bond reuse and unpairing |
| `matter_cec_integration.py` | Operational commands and audio feedback against physical CEC captures |
| `web_hardware.py` | HTTPS guards, configuration, interrupted/full OTA and persistence |
| `web_browser.py` | Actual bundled UI, Enter login, save visibility, manual discovery, diagnostics and session expiry in Chromium |
| `web_activities_ui.py` | Local browser fixture covering saves, recommendations, discovery, pairing UI and session expiry |
| `web_matter_ui.py` + `decode_matter_qr.swift` | Actual pairing toggle and independent QR decoding via macOS Vision |
| `web_onboarding.py` | First-password setup, API guards, stale-tab rejection and reboot persistence |
| `web_matter_coexistence.py` | Concurrent HTTPS traffic and repeated CASE establishment |

Run all runners from this repository root, or pass explicit output paths beneath `test-results/`. Use `--help` for required URL, private boot log and optional fixture addresses. The CEC web runner saves the original configuration before installing its temporary activities; restore that saved file after combined tests. The HID emulator's dependency is installed under the fixture project, not system Python. The native Pi Matter controller and pinned dependencies are documented in [tests/matter_pi.md](tests/matter_pi.md). The earlier AP-only fallback uses the [USB SSH bridge](tests/pi_serial_proxy.md).

## Qualification limits

This validates the supplied ESP32-S3 against Linux CEC and a standard BlueZ HOGP peripheral. A physical Couch Clicker Atomic, commercial TVs/receivers, receiver-specific explicit mute behavior, source sleep/wake behavior, and long-term endurance still need product interoperability qualification. Other ESP32 families are not declared supported. Matter uses development attestation and is not a certified consumer accessory.
