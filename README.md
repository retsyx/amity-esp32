# Amity for ESP32

An ESP32 C++ port of Amity, Matter over Wi-Fi, an optional HomeKit television, and an HTTPS administration UI. This standalone repository contains the ESP32 firmware, provisioning tool, and tests.

## Hardware

The supported development target is the **Seeed XIAO ESP32-S3 with 8 MB flash and 8 MB octal PSRAM**. The S3 offers two CPU cores, enough RAM for simultaneous Matter/BLE/TLS, native USB for installation and recovery, and space for two complete OTA images. The code uses portable ESP-IDF APIs, but C3/C5/C6 and other modules are not declared supported without a target build, an appropriate flash layout, and timing tests on their hardware. PSRAM and the partition layout must not be copied unchanged to a different board.

MbedTLS uses the SDK's external PSRAM allocation mode. Its TLS record buffers
otherwise compete with Matter, WiFi and BLE for internal RAM: on this combined
firmware, internal-only allocation caused HTTPS setup to fail with
`PSA_ERROR_INSUFFICIENT_MEMORY` (`-0x008D`). The full TLS record sizes remain
enabled, including for firmware uploads. The Matter data model also uses
PSRAM, and allocations larger than 1 KiB prefer PSRAM so internal DMA memory
remains available when CASE establishment overlaps HTTPS encryption.

Connect the XIAO's external antenna before commissioning. On the supplied
fixture, attaching it improved home Wi-Fi reception from approximately
−94 dBm to −47 dBm and allowed normal commissioning without a test access point.

Change the two assignments in [main/board_pins.h](main/board_pins.h) for the final board layout:

```c
#define AMITY_CEC_FRONT_GPIO 1  // XIAO D0, TV side
#define AMITY_CEC_BACK_GPIO 2   // XIAO D1, receiver/source side
```

The Amity board2 design has an active-low **FW_RESET** button (SW1) on GPIO4
(XIAO D3, module pad 4), defined by `AMITY_FW_RESET_GPIO` in
`main/board_pins.h`. Current devices have no
button circuitry; support defaults to enabled, and the internal pull-up keeps the
unused input inactive. With a button installed, release it after boot, then hold
it for ten seconds to factory-reset and restart. Short presses do nothing.
The input uses a pull-up and 50 ms software debounce; holding it through reboot
cannot trigger another reset until it has been released.
Disable `CONFIG_AMITY_FW_RESET_BUTTON` under `idf.py menuconfig` → **Amity factory
reset button** if GPIO4 is used by other hardware; disabling leaves the pin untouched.

FW_RESET performs the same factory reset as Advanced in the administration UI:
it clears Wi-Fi credentials, Matter fabrics, HomeKit pairings, remote bonds,
activities, saved device settings, the admin password and TLS identity. It retains
the provisioned factory credentials and installed firmware. The original
provisioned admin password, if supplied, is restored on startup.

Use the Board / Fin electrical interface and CEC pull-ups. Outputs are open-drain, never push-pull. Each bus owns its logical address: a playback address on the front and TV address 0 on the back. The front physical address defaults to `1.0.0.0`. No ARC/eARC or direct-to-TV smart-app routing is added.

Board specifications: [Seeed XIAO ESP32-S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/).

## Build and install

The installed SDKs are expected at `~/.espressif/v6.1/esp-idf` and `~/.espressif/v6.1/esp-matter`. Set `IDF_PATH` and `ESP_MATTER_PATH` before sourcing if installed elsewhere. This port uses the installed IDF 6.1 / Mbed TLS 4 and generated ESP-Matter data model APIs.

```sh
cd ~/projects-other/amity-esp32
source tools/env.sh
idf.py set-target esp32s3
idf.py build provisioning-package
```

The component manager resolves Espressif dependencies and records them in `dependencies.lock`. Generated artifacts remain beneath this repository. The first build of the Matter SDK takes substantially longer than incremental builds.

For a fresh installation, use [amity-provision](amity-provision/README.md) with
`build/amity-firmware.zip`. It produces a complete flash image and a Matter QR
code before installation, so no serial console is needed for onboarding. Flash
the generated image at offset `0x0`, then scan its QR code with your home app.
Keep the device-specific output directory privately. A plain `idf.py flash` on
an erased device is insufficient: fresh firmware requires provisioned credentials.
 An initial administration password can be supplied with `amity-provision --password`,
`--generate-password`, or `--prompt-password`. Without one, open the admin page and create a password before logging in. Until then, only password onboarding is available; all administrative APIs remain locked. Existing passwords survive firmware updates. A self-signed TLS certificate is generated separately for each device and survives reboots. Accept that device's certificate in your browser.

As approved for this port, **Matter commissioning supplies Wi-Fi credentials over BLE**; no separate provisioning app is required. Add the setup code to a Matter controller that supports development/test devices and commission onto a 2.4 GHz network. Then open `https://amity.local` (or `https://<hostname>.local` when provisioned
with `amity-provision --hostname <hostname>`) or the DHCP address printed on the console. The network and commissioning credentials persist across power cycles and firmware updates.

Keep `CONFIG_USE_MINIMAL_MDNS=n`: Matter discovery and HTTPS share Espressif's mDNS responder. Matter owns its operational hostname; the provisioned admin hostname (default `amity.local`) is a delegated alias updated from the station's IPv4 and IPv6 addresses. Running CHIP minimal mDNS alongside the HTTPS responder creates competing UDP 5353 listeners and prevents reliable commissioning.

The firmware uses Espressif's development attestation credentials. Shipping a certified product requires its own vendor/product identity and attestation provisioning. Some consumer ecosystems reject development devices or do not expose all media-player clusters even when commissioning succeeds.

## Administration

* **Activities:** start or repair activities from the compact list, drag the handles to reorder, and use Edit or Add activity to open the editing dialog. The dialog supports optional receiver input fallback. OK stages edits; Cancel discards the dialog draft. Save activities and Discard changes appear only while the list differs from the saved configuration. Discard changes restores the last loaded or saved activities and their order without writing to the device. Start/Repair always targets the saved activity, even after unsaved reordering. Changed or new settings can be run from the dialog with Save and start/repair, which saves all pending activity changes first. Scan HDMI at the top of this tab to inspect freshly discovered device names with role icons and receive editable recommendations. Add recommendations individually; equivalent display/source/audio combinations are excluded, irrespective of name or manual switch settings. On an unconfigured device, the first scan fills in recommendations for review before saving. Device dropdowns filter HDMI scan results by CEC type: Display offers TVs; Source offers playback, recording and tuner devices; Audio offers audio systems and TVs; Switch offers audio systems, HDMI switches and video processors. Each dropdown also includes names configured in that role across all saved activities when their CEC type is unavailable, including offline devices. A known scanned type takes precedence over the saved role. Duplicate and blank suggestions are omitted; manual entry remains available. Device references use OSD names, as in Python Amity; use unique device names.
* **Remote:** Pair Remote automatically pairs the first connectable HID remote with signal strength of at least −50 dBm, matching Python Amity. Bring the remote close and put it in pairing mode. Searching and bonding show status text without a progress indicator; a weak remote triggers “Remote detected. Bring it closer to Amity.” After pairing, the status always shows “Paired”, including while reconnecting; connection details appear only in Advanced → Diagnostics. No device selection is needed. The search lasts 20 seconds and skips remotes that reject bonding. Hold Power + Volume Up on Couch Clicker Atomic for six seconds to clear its old bond. Unpair Remote remains available when the paired remote is asleep or reconnecting.
* **HomeKit:** after Matter commissioning, optionally pair Amity with Apple Home as a television. Start pairing to generate a fresh five-minute QR and PIN in this tab; close pairing or remove HomeKit independently of Matter.
* **Matter:** inspect connected fabrics, toggle a five-minute pairing period with one button, scan the Matter QR shown during pairing, or remove an individual fabric. Fabric removal does not erase activity configuration or the keyboard bond.
* **Advanced:** change the hostname or front CEC physical address, factory-reset to the originally provisioned settings, download/restore configuration, download a bounded in-memory log, upload firmware, change the administration password, or reboot.

Memory summaries are logged once after startup completes and every ten minutes,
without a connected computer. Each `memory` entry includes uptime in seconds and
free bytes, minimum-free bytes since boot, and the largest free block for internal
RAM and PSRAM separately. Minimum-free values sum per-region low watermarks, so
they are conservative bounds rather than simultaneous measurements. Entries appear
on the serial console and in Advanced → Diagnostics → Download logs. The shared
24 KiB log buffer overwrites old entries when full and clears on reboot.

The front CEC physical address defaults to `1.0.0.0`. In Advanced, enter a dotted
hexadecimal address (for example, `2.0.0.0`) or leave the field blank to restore
the default. Saving restarts Amity. This device setting persists across firmware
updates, is excluded from activity backups, and resets to `1.0.0.0` on factory reset.

The web application is served from firmware and needs no Internet/CDN access. API requests require a short-lived bearer session obtained from `/api/login`; passwords are salted and stretched with PBKDF2-HMAC-SHA256. Uploads and all administrative operations use HTTPS. The public `/api/setup` endpoint reports whether onboarding is needed and accepts a password only once. Passwords are never printed to the console or downloadable logs.

The remote's battery level and charging state are also exposed through Matter as
"Remote battery" (Power Source endpoint 3). With no remote paired, it reports
100% and not charging. When a paired remote disconnects, both readings become
unknown. Charging status requires the remote to provide it through its BLE Battery
Service; Amity does not infer it from battery level. Advanced → Diagnostics adds
"Charging" only when charging is reported.

## Remote behavior

Only the Atomic key set is mapped. F1–F4 select activities 1–4; holding selects activities 5–8. A short Power press starts activity 1 from standby or ends the current activity. Holding Power refreshes the current activity, or broadcasts standby when already off. Direction keys always navigate; they never select activities. Source keys go to the current source, volume and mute keys to its audio device. Held buttons repeat as soon as each CEC transmission finishes, subject to normal bus timing; releasing stops repetition and sends a key-release command. Keyboard report descriptors are parsed for both array (6KRO) and bitmap (NKRO) reports; only the known usages produce Amity actions.

BLE and Matter share a single ESP-IDF NimBLE host. The reusable HID component implements HOGP discovery using IDF GATT APIs because the installed `esp_hidh` NimBLE helper skips HID report discovery. It does not patch the SDK. See [BLE component notes](components/ble_remote/README.md).

HID connections use the controller's stable public Bluetooth identity. Matter
keeps its random commissioning advertisement; using that changing address for
HID would create a new peer identity after every reboot and break saved bonds.

HomeKit services and their pairing lifecycle are documented in [HomeKit component notes](components/amity_homekit/README.md).

## Matter data model

| Endpoint | Device type | Controls |
|---|---|---|
| 0 | Root Node | commissioning, fabrics, diagnostics, network |
| 1 | Basic Video Player | On/Off, activity Media Input list, Media Playback, Keypad Input |
| 2 | Speaker | On/Off (mute), relative Level Control Step |
| 3 | Power Source | Remote battery percentage and charge state |

Input indexes match zero-based activity indexes. Media controls enqueue CEC keys. Playback state is the last accepted command; HDMI-CEC cannot supply authoritative play/pause state. Audio reports, when available from the selected audio device, update mute and volume. Unknown absolute volume is null, and unknown mute is reported unavailable. `MoveToLevel` and continuous `Move` are rejected rather than approximated. Explicit mute/unmute use the CEC mute/restore-volume functions, which receivers may not implement. See [Matter component notes](components/amity_matter/README.md).

Activity changes and standby have a settling guard. New activity changes are ignored until setup transmissions and their expected replies finish, followed by five seconds of quiet; press again afterward. Repair remains available and does not start a new settling period. Missing replies time out after two seconds. Configure `CONFIG_AMITY_ACTIVITY_SETTLE_SECONDS` at build time under `idf.py menuconfig` → **Amity activity control**; the default is `5`, and `0` disables the guard. The same rule applies to the remote, web controls and Matter. Saving/restoring configuration is administrative and clears the old activity guard.

Relative volume adjustments are paced to the CEC bus. Changing activity, entering standby, or replacing configuration cancels remaining steps, including queued adjustments from an earlier use of the same activity.

## Persistence and upgrades

`partitions.csv` reserves a 4 KB `amity_factory` partition for commissioning
credentials supplied by the provisioning tool. Existing installations retain
their legacy NVS credentials when upgraded through the admin UI; the partition
table does not need to be changed for those devices.

`partitions.csv` allocates two 3.8125 MiB application slots and a dedicated `amity_cfg` NVS partition. Config, password hash, and TLS identity live there. Wi-Fi credentials, keyboard bonds, and Matter fabric data remain in the normal NVS partition. Initialization never erases a partition automatically to hide an error.

Upload `build/amity.bin` through Advanced → Update Firmware. The uploader validates application identity, chip, size, and the complete ESP image; streams into the inactive slot; and changes the boot selection only after successful validation. Power loss during upload leaves the current application bootable. The bootloader rolls back an unconfirmed image. A new image is confirmed only after configuration, CEC, Matter, BLE, HTTPS, and any saved HomeKit initialization succeed. Never erase the whole flash or change partition offsets for an ordinary upgrade.

The updater shows upload progress, then keeps login hidden while Amity restarts and reconnects. It confirms a new boot with the requested firmware version after startup succeeds, reloads HTML/JavaScript/CSS from the device, and shows “Firmware update complete” with the installed version on the refreshed login page. Reloading during restart resumes the wait; a two-minute timeout offers a reconnection retry, and rollback reports the actual running version. The public `/api/setup` response includes only onboarding state, hostname, firmware version, a per-boot ID and readiness for this check. Older releases without boot-health metadata offer login to verify the version instead of claiming completion. The updated updater is available after installing 0.1.41 and reloading the admin page.

The browser reads the version embedded in the selected binary before uploading. Older versions require an explicit warning confirmation: downgrades are unsupported and at the user's own risk. Versions use semantic-version ordering (including prereleases; build metadata does not affect order). Unrecognized versions also require risk confirmation. The device independently checks the version before erasing flash; raw API clients must send `X-Amity-Confirm-Version: <currently running version>` to explicitly approve a downgrade or an unrecognized version. The request body remains the original `.bin` file. This confirmation does not override image validation or make older firmware compatible with newer saved data.

Set release versions at configure time, for example `idf.py -DPROJECT_VER=0.2.0 -DPROJECT_VER_NUMBER=25 reconfigure build`. OTA images must be built for the same target and partition scheme.

Matter reports manufacturer `Amity` and model `Amity ESP32`, configured in `CHIPProjectConfig.h`.

Configuration backups use versioned JSON and contain user activities and optional vendor quirks, **not** hostnames, front CEC physical addresses, initial passwords, Wi-Fi passwords, administrator credentials, TLS keys, BLE bonds, Matter fabrics, or HomeKit credentials. Restore validates the complete document before committing. See [example-config.json](example-config.json). Python YAML backups are not accepted directly; copy the activity fields into the documented JSON format.

## Components and validation

| Component | Responsibility |
|---|---|
| `cec` | generic CEC timing, arbitration, ACK, retries, GPIO transport |
| `amity_hub` | Python-compatible dual-bus identities, discovery, activities, routing, source suppression |
| `ble_remote` | shared-host HID discovery, bonding, report parsing, Atomic key mapping |
| `amity_matter` | Matter endpoints, delegates, commissioning credentials and fabric management |
| `amity_homekit` | Optional HomeKit television, speaker, activity inputs, battery and pairing lifecycle |
| `config_store` | bounded atomic NVS storage |
| `web` | generic TLS HTTP helpers, authentication, request handling |
| `web_admin` | administration pages and Amity APIs |
| `system_services` | bounded log capture and streaming OTA |
| `device_settings` | immutable provisioning, hostname/CEC address overrides, resumable factory reset |
| `main` | composition, pins, key hold/repeat behavior |

Run host tests after SDK dependency configuration:

```sh
sh tests/run_host_tests.sh
```

Tests compile CEC, hub, HID parsing, and configuration validation with AddressSanitizer and UndefinedBehaviorSanitizer. Hardware procedures and recorded results are in [VALIDATION.md](VALIDATION.md). Unit tests alone do not establish electrical compliance or interoperability with an actual receiver.

## Licenses

The Amity port derives its behavior from this repository's GPLv3 Amity implementation and follows the repository license. No claim is made that the Amity application itself is closed-source licensed. Additional runtime dependencies are Espressif/CHIP SDK components (Apache-2.0/BSD-style licenses) cJSON (MIT), Espressif HomeKit components (MIT/Apache-2.0/BSD), libsodium (ISC), and the SDK-bundled Nayuki QR encoder (MIT). The optional Pi keyboard test emulator uses dbus-next (MIT). See [THIRD_PARTY.md](THIRD_PARTY.md).
