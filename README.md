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

Amity is designed for a traditional setup centered around an audio/video receiver (AVR). For example, a TV connected to a receiver output, and various playback devices (i.e. media streamers, blu-ray players, and game consoles, etc.) connected to the receiver's inputs. When changing the source, the input is changed on the receiver. The HDMI-CEC protocol only allows the designated TV to select sources or devices to declare themselves as active sources. TVs do not heed arbitrary source selection commands from other devices. As a result, Amity cannot select inputs on a TV, and cannot control sources connected directly to a TV, including built-in smart TV apps. Similarly, Amity does not support HDMI ARC/eARC. If you use smart TV apps or connect devices, other than a receiver, directly to the TV inputs, Amity is not for your system.

Amity can only control devices that support HDMI-CEC! However, devices that don't support HDMI-CEC (old game consoles, for example) can still be facilitated.




### Development Status

HDMI-CEC implementations vary between manufacturers, and this version of Amity has been tested with a limited range of equipment. It may work well with your system, or particular devices may need workarounds. See the [validation record](https://github.com/retsyx/amity-esp32/blob/main/VALIDATION.md) for the checks performed on this version.

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

**Installing this full image replaces all existing settings and pairings.** Use it for initial installation. Later browser updates use a different file, `amity.bin`.

When installation finishes, leave the board powered on, or connect it to its USB-C power supply.

### HDMI Connections

Amity connects between the TV and the receiver.

1. Disconnect the HDMI cable connecting the TV's input to the receiver's output.
2. Connect the TV's HDMI input to the Amity HDMI port marked `TV`.
3. Use a second HDMI cable to connect the receiver's HDMI output to the Amity HDMI port marked `AVR`.
4. Leave the playback devices connected to the receiver's inputs.

Enable HDMI-CEC on the TV, receiver, and playback devices. Manufacturers use different names for this setting; check your equipment's setup menus.

### Wi-Fi and Browser Access

Matter pairing supplies Amity with its Wi-Fi settings. Complete this step even if you intend to use HomeKit for everyday control.

1. In your Matter home app, add a device and scan the QR code in `amity-pairing.png`. Alternatively, enter the manual pairing code from `amity-pairing.txt`.
2. Follow the app's prompts to connect Amity to your 2.4 GHz Wi-Fi network. If the initial pairing window expires, restart the unpaired board and try again.
3. On a phone or computer connected to the same network, open the administration address on the setup card. For the example above, it is `https://living-room.local`. The default hostname gives `https://amity.local`.
4. The browser will show a certificate warning because Amity creates its own certificate for encryption. Open the warning's details and accept the certificate to continue.
5. Log in with the generated admin password. If you prepared the image without an initial password, create one when prompted.

If the hostname cannot be found, locate Amity in your router's device list and browse to `https://IP_ADDRESS` instead.

### Pairing a Remote

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

The activity fields are:

* `Activity name` — a descriptive name of your choice.
* `Display` — the screen, typically your TV.
* `Source` — the playback device, such as a streamer or game console.
* `Audio` — the device that receives volume and mute commands, typically your receiver.
* `Switch device` — an optional receiver or switch used for explicit input selection.
* `Switch input` — the input number, when a switch device is specified. See [Non-CEC Sources](#non-cec-sources).

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

These numbers refer to the order in the 'Activities' tab. For example, Siri Remote Select starts the first activity, Up starts the second, and a long press of Select starts the sixth.

A long press of **Power** repeats the standby commands. This is useful if a device is still on when it should be off.

### Active

During an activity, navigation and playback buttons control the source. Volume and mute control the audio device. For example, in a PlayStation activity, the directional buttons navigate the PlayStation interface, while the volume buttons control the receiver. Siri Remote touchpad swipes also navigate the source's menus.

A short press of **Power** ends the activity and puts the system in standby. A long press repeats the current activity's setup. Use this if a device failed to turn on or selected the wrong input. The 'Repair' button in the browser performs the same action.

To change activities on Siri Remote, hold **Power** and press an activity-selection button. Atomic uses its dedicated activity buttons. You can also select an activity in the browser.

Give your equipment a few seconds to finish switching before choosing a different activity. If Amity ignores the selection while the system is settling, press again after the transition finishes.

## Non-CEC Sources

A source without HDMI-CEC can still be used if your receiver supports input selection over HDMI-CEC. Amity can turn on the TV and receiver, select the receiver input, and control volume. Use the source's own controls for its power, navigation, and playback.

Create an activity with these settings:

1. Select the TV as `Display`.
2. Select the receiver as `Audio`.
3. Leave `Source` blank.
4. Select the receiver as `Switch device`.
5. Set `Switch input` to the receiver's input number.
6. Press 'OK', then 'Save activities'.

Input numbers vary by receiver. Consult its documentation or setup menus for the correct value. This option can also help with a source that does not reliably appear in HDMI scans.

## Matter

The initial Matter pairing connects Amity to Wi-Fi and adds it to your home controller. Once activities are configured, the controller can use Amity for home automation.

The 'Matter' tab lists controller connections. To add another controller, press 'Start pairing' and scan the displayed QR code in that controller's home app. The pairing window lasts five minutes.

Available controls may include power, activity selection, playback, volume, mute, and remote battery status. What appears depends on the home app and your equipment. Removing a Matter connection leaves your activities and remote pairing intact.

## HomeKit

Amity can also be added to Apple Home as a television accessory, allowing control through Siri and HomeKit automations.

Complete Wi-Fi setup and activity configuration first. Select the 'HomeKit' tab, press 'Start pairing', and scan its QR code in Apple Home. The pairing window lasts five minutes.

HomeKit has its own pairing code, separate from Matter. Removing the HomeKit association leaves Matter connections and activities intact.

## Backup & Restore

Select the 'Advanced' tab. In the 'Configuration' section, press 'Download' to save a backup, or use the upload field to restore one.

The backup contains activities and device-specific control settings. It does not contain Wi-Fi settings, admin credentials, pairings, hostname changes, or the HDMI-CEC address. Keep the original installation folder as well; a configuration backup does not replace your pairing card or USB installation image.

## Firmware Updates

Read the new release's notes before updating, especially before version 1.0. Normal updates preserve settings, pairings, passwords, and the original Matter setup code, but a release may require a fresh installation. Downgrades are unsupported.

1. Download `amity.bin` from the [release's Assets](https://github.com/retsyx/amity-esp32/releases), or use `build/amity.bin` from a local build.
2. Select the 'Advanced' tab, and find 'Update Firmware'.
3. Select the file and confirm the update.
4. Keep Amity powered on and the page open until completion is reported, then log in again.

Use `amity.bin` for browser updates. The full `amity-provisioned.bin` image is for USB installation.

## Factory Reset

In the 'Advanced' tab, select 'Factory Reset' and confirm. This clears activities, Wi-Fi settings, Matter and HomeKit pairings, and the remote bond. It also restores the original hostname and initial admin password, if one was supplied.

The installed firmware and original Matter pairing code remain. Add Amity to your home again using the original setup card, then pair the remote and configure activities.

To reset using the board's **FW RESET** button, power on Amity with the button released, then hold it for ten seconds. This performs the same factory reset. FW RESET is separate from the module's restart and boot buttons.

If you cannot access the administration page, you can also reinstall the saved USB image. This replaces settings and pairings with the original installation state.

## Troubleshooting

* **The computer does not see Amity:** use a USB data cable, and check that you selected the board's serial port.
* **The setup tool does not start:** check that you downloaded the archive for your OS and processor. On macOS/Linux, 'Permission denied' may require `chmod +x amity-provision`. These development executables are unsigned, so your OS may require approval to run them.
* **Initial Matter pairing fails:** check the antenna connection and the phone's Bluetooth setting. Restart the unpaired board to reopen pairing, and check that the controller accepts development Matter devices.
* **The browser cannot find Amity:** use the address on the setup card from the same network, or use the IP address shown by your router.
* **Equipment is missing or on the wrong input:** check HDMI connections and HDMI-CEC settings, scan again, and review the activity's device selections. If the activity is already running, try 'Repair'.
* **The paired remote does not respond:** wake it, move it closer to Amity, and check 'Advanced' → 'Diagnostics'.

To download troubleshooting logs, select 'Advanced' → 'Diagnostics' → 'Download logs'.

Amity determines its front HDMI-CEC physical address on every startup from the
lowest valid address reported by back-side devices. If none is reported, it uses
the 'Default front physical address' setting in 'Advanced' → 'HDMI-CEC'
(`1.0.0.0` by default). 'Advanced' → 'Diagnostics' shows the result for the current
boot. This is a best-effort estimate: a silent AVR can leave only a child device's
address available. A new scan keeps the current address; restarting determines
it again.

## License

Amity source code is licensed under the [GPLv3](https://github.com/retsyx/amity-esp32/blob/main/LICENSE). See the [third-party notices](https://github.com/retsyx/amity-esp32/blob/main/THIRD_PARTY.md) for dependency licenses.
