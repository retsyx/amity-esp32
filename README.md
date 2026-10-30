# Amity for ESP32

HDMI-CEC that works!

Control your home theater with a familiar remote, and integrate it with Matter or HomeKit.

This version runs on the Amity ESP32 board. The [original Amity](https://github.com/retsyx/amity) runs on a Raspberry Pi.

## Prerequisites

* An assembled [Amity ESP32 board](https://github.com/retsyx/amity-esp32/tree/main/hw/board) and a USB-C power supply.
* An extra HDMI cable. Amity uses two cables in place of the one connecting your receiver to your TV.
* A remote control: [Couch Clicker Atomic](https://github.com/retsyx/CouchClickerAtomic), or a second- or third-generation Siri Remote.

## Caveats

### Amity is an HDMI-CEC Controller (Not a Universal Remote)

Amity uses HDMI-CEC to control the equipment selected in each activity. Display, source, audio and optional receiver-input choices keep the original Amity behavior. Equipment on another Amity device is addressed by that device, HDMI adapter, CEC side and OSD name; its owner performs the same local handling. Equipment can remain selected while it is powered off or unavailable.

Amity can only control devices that support HDMI-CEC! However, devices that don't support HDMI-CEC (old game consoles, for example) can still be facilitated.




### Development Status

The development implementation follows the [multi-device control design](amity-octopus-consortium/README.md): current-format activity configuration, scoped equipment lookup, original local activity handling, authenticated enrollment and owner delivery, explicit device recovery, and manifest-bound updates. The existing board defaults to **one HDMI adapter (two isolated CEC ports)**. A separate development profile supports **up to four optional HDMI adapters per device**, with a software limit of **three active devices per system**. Each device uses one shared CEC timer. Historical bench evidence covers one device and an independent passive peer; group capacity, four-adapter electrical timing, AV equipment, production security provisioning and observed novice usability need their own qualification evidence. See [implementation status](IMPLEMENTATION_STATUS.json) for exact checks and remaining qualification gates.

The original first-setup Amity remains the controller, hosts the only browser UI and Matter/Apple Home integration, and pairs with the sole remote. Other Amity devices are subordinate actuators with no UI. The one remote and browser can select activities for multiple screens. Amity device names equal their actual hostnames. Add requires a valid Name and applies it to the added device; main hostname changes remain in Advanced. Names are not separate cosmetic aliases, while equipment selectors, activity IDs, home-app slots and pairings remain stable. No saved current-activity/resume journal or missed input is replayed after restart.

Configuration, discovery, received frames and explicit activity selection retain the original local Amity handling. Additional devices provide ownership and delivery; they do not introduce an activity-enable switch, receiver-profile approval or a new HDMI-path setup workflow.

HDMI-CEC implementations vary between manufacturers, and this version of Amity has been tested with a limited range of equipment. It may work well with your system, or particular devices may need workarounds. See the [implementation and validation status](IMPLEMENTATION_STATUS.json) for the recorded checks and remaining qualification gates.

Amity is not a certified Matter accessory. Some home apps may reject it or expose only some of its controls.

Before version 1.0, compatibility with earlier Amity releases is not maintained. Read the release notes before updating. A release may require a fresh installation and pairing again.

## Setup

### HDMI Hardware

The Amity board connects between the TV and the receiver, allowing Amity to control the devices on either side. Attach the external antenna before powering the board.

### Initial Installation

For a new board, use the setup tool to prepare its firmware image and Matter pairing card, then install the image over USB. If your board already has Amity installed, continue with [HDMI Connections](#hdmi-connections). To update an existing installation, see [Firmware Updates](#firmware-updates).

#### Download a Release

1. Choose a version from [GitHub Releases](https://github.com/retsyx/amity-esp32/releases), and read its installation notes.
2. From the release's **Assets**, download `amity-firmware.zip` and the setup-tool ZIP for your computer:

   | Computer | Setup tool |
   |---|---|
   | Apple Silicon Mac | `amity-provision-darwin-arm64.zip` |
   | Intel Mac | `amity-provision-darwin-amd64.zip` |
   | Windows, Intel/AMD | `amity-provision-windows-amd64.zip` |
   | Windows on ARM | `amity-provision-windows-arm64.zip` |
   | Linux, Intel/AMD | `amity-provision-linux-amd64.zip` |
   | Linux, ARM64 | `amity-provision-linux-arm64.zip` |

3. Extract the setup-tool ZIP into the same folder as `amity-firmware.zip`. Leave the firmware ZIP intact.
4. Open a terminal in that folder, and run the command below.

On macOS or Linux:

```sh
./amity-provision --hostname living-room --generate-password --out living-room amity-firmware.zip
```

On Windows, use PowerShell:

```powershell
.\amity-provision.exe --hostname living-room --generate-password --out living-room amity-firmware.zip
```

Replace `living-room` with a memorable name for your Amity. Choose a distinct name and output folder for each board.

The tool creates the `living-room` folder, containing:

* `amity-provisioned.bin` — the complete image for USB installation.
* `amity-pairing.png` — a setup card with the Matter QR code, manual pairing code, browser address, and generated admin password.
* Text files with the same setup details and password.

Keep this folder private, and save a backup. You will need the original Matter code again after a factory reset. The admin password is separate from the Matter pairing code.

The setup tool runs without installing development SDKs. For other options, including choosing your own password, see the [setup-tool guide](https://github.com/retsyx/amity-esp32/blob/main/amity-provision/README.md). The release's `SHA256SUMS` file provides checksums for the downloads.

Continue with [Install over USB](#install-over-usb).

#### Build Locally

To build your own firmware, follow the [local build guide](https://github.com/retsyx/amity-esp32/blob/main/BUILDING.md). It covers macOS, Linux, and WSL2 on Windows, including how to generate the installation image and pairing card.

Once your board's files are prepared, continue below.

#### Install over USB

1. Connect the Amity board to your computer with a USB data cable.
2. Install [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html).
3. Find the board's serial port. Typical names are `COM3` on Windows, `/dev/cu.usbmodem…` on macOS, and `/dev/ttyACM0` on Linux.
4. Run this command, replacing `PORT` with the serial port and `living-room` with your output folder:

```sh
esptool --chip esp32s3 --port PORT write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 living-room/amity-provisioned.bin
```

If the `esptool` command is unavailable, use `python3 -m esptool` on macOS/Linux, or `python -m esptool` on Windows, in its place.

**Installing this full image replaces all existing settings and pairings.** Use it for initial installation. Later browser updates use a different file, `amity-update.bin`.

When installation finishes, leave the board powered on, or connect it to its USB-C power supply.

Matter setup opens automatically at boot on a fresh board or original main with no current Matter pairings; no button press is required. This includes factory-reset and unclaimed reboots. A main whose last Matter pairing was removed keeps its main role and opens setup again at boot. Boards with a current Matter pairing and added Amity devices do not reopen setup on reboot.

To add an unclaimed board to an existing system, open **Amity devices → Add** on the main, hold the new board’s **FW RESET** for at least three seconds, and release before ten seconds. Choose it if several boards appear, enter its name, and add it. Holding for ten seconds performs factory reset.

Adding devices requires updated firmware on both boards. Normal setup needs no code. If Amity asks for one, use the new board's existing Matter manual code or QR payload. Existing Matter setup cards remain valid. Adding a device does not enroll it in a Matter home.

### HDMI Connections

Amity connects between the TV and the receiver.

1. Disconnect the HDMI cable connecting the TV's input to the receiver's output.
2. Connect the TV's HDMI input to the Amity HDMI port marked `TV`.
3. Use a second HDMI cable to connect the receiver's HDMI output to the Amity HDMI port marked `AVR`.
4. Leave the playback devices connected to the receiver's inputs.

Enable HDMI-CEC on the TV, receiver, and playback devices. Manufacturers use different names for this setting; check your equipment's setup menus.

On hardware with several HDMI adapters, unused connections can stay empty. Adapter numbers stay fixed when another adapter is removed. Saved equipment and activities remain available for repair; reconnecting equipment does not start an activity automatically. Use **Scan HDMI**, then explicitly select or start the activity when ready. Attachment status requires suitable detection hardware; otherwise Amity reports equipment availability from discovery.

### Wi-Fi and Browser Access

Matter pairing supplies Amity with its Wi-Fi settings. Complete this step even if you intend to use HomeKit for everyday control.

1. Power on the board you want to use as the main. A fresh board or original main with no current Matter pairings automatically opens Matter setup. In your Matter home app, add a device and scan the QR code in `amity-pairing.png`, or enter the manual pairing code from `amity-pairing.txt`. Add another board through **Amity devices → Add** on the original controller instead.
2. Follow the app's prompts to connect Amity to your 2.4 GHz Wi-Fi network. If its setup window expires, restart it to reopen setup while it still has no Matter pairings. A short FW RESET press also reopens initial setup on an unclaimed board. Holding FW RESET for ten seconds performs a factory reset.
3. On a phone or computer connected to the same network, open the administration address on the setup card. For the example above, it is `https://living-room.local`. The default hostname gives `https://amity.local`.
4. The browser will show a certificate warning because Amity creates its own certificate for encryption. Open the warning's details and accept the certificate to continue.
5. Log in with the generated admin password. If you prepared the image without an initial password, create one when prompted.

If the hostname cannot be found, locate Amity in your router's device list and browse to `https://IP_ADDRESS` instead.

### Pairing a Remote

Pair the sole remote to the original Amity device. Added devices cannot own a remote; choosing another screen does not move pairing ownership.

Charge the remote, and unpair it from any other device before pairing it with Amity.

1. Select the 'Remote' tab, and press 'Pair remote'.
2. Hold the remote close to the Amity board.
3. Start pairing on the remote:

   | Remote | Pairing buttons |
   |---|---|
   | Couch Clicker Atomic | Hold **Power + Volume Up** for six seconds to clear the previous bond and pair again. [Remote guide](https://github.com/retsyx/CouchClickerAtomic) |
   | Siri Remote, generation 2 or 3 | Hold **Back + Volume Up** for five seconds. [Apple's pairing instructions](https://support.apple.com/en-us/102569) |

4. Wait for Amity to report that the remote is paired.

Amity selects a nearby supported remote automatically. If pairing times out, move the remote closer and try again. To replace a remote, press 'Unpair remote' before pairing the new one.

A sleeping remote remains paired, and reconnects when it wakes.

### Activity Configuration

An activity describes a use of your home theater, such as 'Watch Apple TV' or 'Play PlayStation'. It specifies a display, a source, and an audio device. Selecting the activity turns on the appropriate equipment and routes the remote's commands to those devices.

Ensure that HDMI-CEC is enabled on all equipment, select the 'Activities' tab, and press 'Scan HDMI'. On a new installation, Amity creates a suggested list of activities from the devices it finds. Review the names and device selections, then press 'Save activities'.

Devices are identified by their HDMI On Screen Display (OSD) names. Each device must have a distinct name. If two devices report the same name, change one in that device's settings before configuring activities.

If a device is missing, check its HDMI-CEC setting and connections, then scan again. Some devices only appear while powered on. A source that does not reliably appear can sometimes be used through [receiver input selection](#non-cec-sources).

#### Editing the List of Activities

Amity supports up to 16 activities. Drag activities to change their order. The position in the list determines which remote shortcut starts the activity; see [Usage](#usage). All configured activities can also be started from the browser.

Use 'Add activity' to create an activity, 'Edit' to change one, and 'Delete' to remove one. Press 'Save activities' when finished. 'Discard changes' returns to the saved configuration.

#### Editing an Activity

The activity fields appear in one column:

* `Name` — a descriptive name of your choice.
* `Display` — the screen, typically your TV.
* `Source` — the playback device, such as a streamer or game console.
* `Audio` — the device that receives volume and mute commands, typically your receiver.
* `Switch Device` — an optional receiver or switch used for explicit input selection.
* `Input` — the input number, shown only when a switch device is specified. See [Non-CEC Sources](#non-cec-sources).

Press 'OK' to accept the edit, then 'Save activities' to save it. 'OK' alone does not save the configuration.

## Usage

Amity has two basic modes: standby, when no activity is selected, and active, when an activity is running.

Simple button presses are enough for normal use. Long presses and button combinations provide additional shortcuts, but family members do not need to learn them to use Amity.

### Standby

A short press of **Power** starts the first activity. Other activity shortcuts depend on your remote:

| Remote | Short press | Long press |
|---|---|---|
| Atomic activity buttons 1–4 | Activities 1–4 | Activities 5–8 |
| Siri Remote Select, Up, Right, Down, Left | Activities 1–5, in that order | Activities 6–10, in that order |

These shortcuts select activities by their current order in the list. The remote remains on the original controller while selecting activities for different screens.

A long press of **Power** repeats the standby commands. This is useful if a device is still on when it should be off.

### Active

During an activity, navigation and playback buttons control the source. Volume and mute control the audio device. For example, in a PlayStation activity, the directional buttons navigate the PlayStation interface, while the volume buttons control the receiver. Siri Remote touchpad swipes also navigate the source's menus.

A short press of **Power** ends the activity and puts the system in standby. A long press repeats the current activity's setup. Use this if a device failed to turn on or selected the wrong input. The 'Repair' button in the browser performs the same action.

To change activities on Siri Remote, hold **Power** and press an activity-selection button. Atomic uses its dedicated activity buttons. You can also select an activity in the browser.

Give your equipment a few seconds to finish switching before choosing a different activity. If Amity ignores the selection while the system is settling, press again after the transition finishes.

## Non-CEC Sources

For a source without HDMI-CEC, select its receiver or switch in **Switch Device** and enter the receiver input number in **Input**. Amity retains its original explicit receiver-input fallback when the source cannot be found. Use the source's own controls for its power, navigation and playback. The receiver must support the corresponding CEC input command.

## Matter

The initial Matter pairing connects Amity to Wi-Fi and adds it to your home controller. Once activities are configured, the controller can use Amity for home automation.

**Matter** lists controller connections. To add another controller, press 'Start pairing' and scan the displayed QR code in that controller's home app. The pairing window lasts five minutes.

Available controls may include power, activity selection, playback, volume, mute, and remote battery status. What appears depends on the home app and your equipment. Removing a Matter connection leaves your activities and remote pairing intact.

## HomeKit

Amity can also be added to Apple Home as a television accessory, allowing control through Siri and HomeKit automations.

Complete Wi-Fi setup and activity configuration first. Open **HomeKit**, press 'Start pairing', and scan its QR code in Apple Home. The pairing window lasts five minutes.

HomeKit has its own pairing code, separate from Matter. Removing the HomeKit association leaves Matter connections and activities intact.

## Backup & Restore

Open **Advanced**. In the 'Configuration' section, press 'Download' to save a portable backup, or select a backup to preview its import. For replacement hardware, explicitly map saved equipment owners to the replacement devices. Missing equipment remains selected and uses ordinary lookup when an activity runs.

The backup contains activities and device-specific control settings. It does not contain Wi-Fi settings, admin credentials, pairings, hostname changes, or the HDMI-CEC address. Keep the original installation folder as well; a configuration backup does not replace your pairing card or USB installation image.

If the main Amity fails, disconnect/isolate it, set up replacement hardware through Matter BLE, and use the existing **Advanced** configuration upload/restore to recover saved activity/device IDs, names and order. Pair the remote and home integrations again, and factory-reset each other Amity device with the existing ten-second button hold, then open its three-to-below-ten-second Add window and re-add it through the replacement main. Its hardware identity and original Matter setup code survive reset; Wi-Fi and old operational trust are cleared. The backup does not copy keys, pairings or trust in the old main. Restored activities retain their original equipment and receiver-input choices. Runtime lookup reports unavailable owners or equipment without permanently disabling an activity. Restoration does not start an activity. This is manual recovery, with no automatic failover or live authority transfer.

## Firmware Updates

Read the new release's notes before updating, especially before version 1.0. Normal updates preserve settings, pairings, passwords, and the original Matter setup code, but a release may require a fresh installation. Downgrades are unsupported.

The qualified-lookup release uses a new saved configuration format. Existing **0.1.80** devices need a USB firmware upgrade that preserves their factory data and settings; their current browser updater rejects the new format. Follow the [existing-device upgrade instructions](BUILDING.md#upgrading-an-existing-0180-installation-to-schema-3), and do not flash a full fresh-install image over existing settings.

1. Download `amity-update.bin` from the release. This one file contains the unchanged application image, its exact manifest, and the publisher signature for a signed release. Local development builds produce an unsigned update file.
2. Open **Advanced → Update Firmware** and select `amity-update.bin`. Review the version and confirm installation.
3. Keep Amity powered and the page open while it uploads and restarts. The page waits for a new boot and local health. An interrupted upload keeps the current image; reselect the same update file to retry.

The full `amity-provisioned.bin` image is for USB installation. `amity.bin` is the raw application for USB/debug tooling; raw browser uploads require its exact manifest to have been validated separately. The manifest and optional `amity.release.sig` are also published for release inspection. An older development installer that predates update bundles may need a supported bridge release. A separately generated compatibility manifest (`tools/release_manifest.py --compatibility-capabilities 511`) changes no image bytes; signed compatibility manifests must be produced and signed by the publisher.

Production builds require a separate P-256 publisher key, native secure boot, release-mode flash encryption and provisioned encrypted NVS. `tools/production_profile.py` prepares and audits an offline build profile without flashing or programming eFuses. Its `derive-storage-keys` command creates private per-partition NVS key files from provisioned master material; firmware uses the same HKDF scheme. Native signing and publisher signing use separate keys. The baseline fresh-install provisioner rejects the encrypted production layout; production provisioning and physical recovery must be qualified separately on disposable hardware.

## Factory Reset

In **Advanced**, select 'Factory Reset' and confirm. This clears activities, Wi-Fi settings, Matter and HomeKit pairings, and the remote bond. It also restores the original hostname and initial admin password, if one was supplied.

To remove an added device, open **Amity devices** and select **Remove** beside
its name. A responding Amity is factory-reset and restarted after removal. If it
is not responding, it is dropped from the main's system; hold that device's
**FW RESET** button for at least ten seconds to reset it physically. If reset
could not be confirmed, check the result and use the physical button as needed.
Saved equipment and activities remain unavailable for repair or replacement.
This action does not reset the main Amity or other devices. Network loss alone
never factory-resets an Amity.

The installed firmware and original Matter pairing code remain. Add Amity to your home again using the original setup card, then pair the remote and configure activities.

To reset using the board's **FW RESET** button, power on Amity with the button released, then hold it for ten seconds. This performs the same factory reset. FW RESET is separate from the module's restart and boot buttons.

If you cannot access the administration page, you can also reinstall the saved USB image. This replaces settings and pairings with the original installation state.

## Troubleshooting

* **The computer does not see Amity:** use a USB data cable, and check that you selected the board's serial port.
* **The setup tool does not start:** check that you downloaded the archive for your OS and processor. On macOS/Linux, 'Permission denied' may require `chmod +x amity-provision`. These development executables are unsigned, so your OS may require approval to run them.
* **Initial Matter pairing fails:** check the antenna connection and the phone's Bluetooth setting. A fresh board or original main with no current Matter pairings automatically opens its bounded setup window at boot; restart it if that window expired, and check that the controller accepts development Matter devices. An existing main can also reopen pairing through **Matter → Start pairing**. Added Amity devices stay headless and do not open Matter setup on reboot.
* **The browser cannot find Amity:** use the address on the setup card from the same network, or use the IP address shown by your router.
* **Equipment is missing or on the wrong input:** check HDMI connections and HDMI-CEC settings, scan again, and review the activity's device selections. If the activity is already running, try 'Repair'.
* **The paired remote does not respond:** wake it, move it closer to Amity, and check 'Advanced' → 'Diagnostics'.

To download troubleshooting logs, select 'Advanced' → 'Diagnostics' → 'Download logs'.

Open the **Amity devices** tab and select **Settings** beside the named Amity to change
an HDMI adapter's default front physical address (`1.0.0.0` initially; blank
restores it). A usable detected address takes priority. The dialog separates the
saved default, detected address and selected front address; a selected fallback
does not confirm the HDMI route or start an activity. Stop the selected device's
activities before saving. Saving restarts only that Amity, and activities remain
paused afterward. Opening the dialog does not scan or wake equipment.

HDMI adapters retain socket numbers 1–4 within the device's supported capacity.
Removing adapter 2 does not rename adapter 3 or copy its settings. Missing and
unconfigured adapters keep their saved defaults. A detachable adapter can be
edited only when a configured presence detector confirms it is attached; an
adapter without a detector has unknown presence and is read only. The original
fixed one-adapter hardware is present by construction. Detachable four-adapter
hardware and its presence wiring still require physical qualification.

## License

Amity source code is licensed under the [GPLv3](https://github.com/retsyx/amity-esp32/blob/main/LICENSE). See the [third-party notices](https://github.com/retsyx/amity-esp32/blob/main/THIRD_PARTY.md) for dependency licenses.

The 20 October 2026 UI amendment preserves the original Activities, Remote, Matter, HomeKit and Advanced workflows. Device listings add canonical Amity-hostname/HDMI-adapter context, and one Amity tab manages added devices. No guided activity wizard, new remote owner mapping, primary-zone picker or member-hosted UI is part of this scope.
