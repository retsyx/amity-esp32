# Amity for ESP32

HDMI-CEC that works!

Control your home theater with a familiar remote, and integrate it with Matter and HomeKit home automation.

Read the [rationale](doc/rationale.md), to know why Amity uses HDMI-CEC, and how it works.

This version runs on the Amity ESP32 board. The [original Amity](https://github.com/retsyx/amity) runs on a Raspberry Pi.

## Prerequisites

* An [Amity ESP32 board](hw/README.md) and a USB-C power supply.
* An extra HDMI cable. Amity uses two cables in place of the one connecting your receiver to your TV.
* A remote control. [Couch Clicker Atomic](https://github.com/retsyx/CouchClickerAtomic), [Couch Clicker Atomic 2](https://github.com/retsyx/CouchClickerAtomic2) or a Gen 2/3/4 Siri Remote.
* A Matter compatible home automation system.

## Amity is a flexible HDMI-CEC Controller (Not a Universal Remote)

### The Amity Adapter

Amity connects between home theater componenets using an HDMI Amity adapter. The adapter intercepts HDMI-CEC signals while passing through the HDMI A/V signals untouched. Each adapter has a `Display` HDMI connector that is connected to the display side and a `Source` HDMI connector that is connected to A/V source side.

### Single Adapter Amity

The single adapter Amity is designed for a traditional setup centered around an audio/video receiver (AVR). For example, a TV connected to a receiver output, and various playback devices (i.e. media streamers, blu-ray players, and game consoles, etc.) connected to the receiver's inputs. When changing the source, the input is changed on the receiver. The HDMI-CEC protocol only allows the designated TV to select sources or devices to declare themselves as active sources. TVs do not heed arbitrary source selection commands from other devices. As a result, Amity cannot select inputs on a TV, and cannot control sources connected directly to a TV, including built-in smart TV apps. Similarly, the singla adapter Amity does not support HDMI ARC/eARC. If you use smart TV apps or connect devices, other than a receiver, directly to the TV inputs, Amity is not for your system.

### Multi-Adapter Amity

Amity firmware is designed to support up to four Amity adapters. A multi-adapter Amity enables more applications. It can be used to control a system with two displays (for example, a TV and a projector) connected to a single receiver and to control a system without a receiver, where a TV is connected directly to multiple components, or where some components are connected to the TV and some are connected to a receiver. It can also be used to isolate particularly misbehaved components that demand to be displayed when not appropriate.

### Multi-Amity

Up to four Amities (with four adapters each) can be networked together over WiFi to control a large home theater setup containing more components than a single Amity can control or to control multiple physically separate systems with a single remote.

### Controlling Components Without HDMI-CEC

Amity can only control devices that support HDMI-CEC! However, devices that don't support HDMI-CEC (old game consoles, for example) can still be facilitated.

## Initial Amity Firmware Setup

For a new board, use the provisioning tool to prepare a firmware image and setup card, then install the image over USB. If your board already has Amity installed, continue with [Setup](#setup). To update an existing Amity, see [Firmware Updates](#firmware-updates).

### Download

1. Go to the latest [Amity Release](https://github.com/retsyx/amity-esp32/releases/latest).
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
./amity-provision --hostname <amity hostname> --generate-password --out <output folder> amity-firmware.zip
```

On Windows, use PowerShell:

```powershell
.\amity-provision.exe --hostname <amity hostname> --generate-password --out <output folder> amity-firmware.zip
```

Choose a distinct name and output folder for each provisioned board.

The tool creates the output folder, containing:

* `amity-provisioned.bin` — the firmware image for USB installation.
* `amity-pairing.png` — a setup card with the Matter pairing QR code, Matter pairing code, browser address, and generated admin password.
* Text files with the same setup details and password.

For other options, including choosing your own password, see the [Amity provisioning guide](amity-provision/README.md).

If you lose the provisioning information for the board, you will need re-provision, wipe, and re-install the board from scratch!

Continue with [Initial Install over USB](#initial-install-over-usb).

### Build Locally

To build your own firmware, follow the [local build guide](BUILDING.md).

### Initial Install Over USB

1. Connect the Amity board to your computer with a USB data cable.
2. Install [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html).
3. Find the board's serial port. Typical names are `COM3` on Windows, `/dev/cu.usbmodem...` on macOS, and `/dev/ttyACM0` on Linux.
4. Run this command with your provisioning output folder:

```sh
esptool --chip esp32s3 --port <port>> write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 <output folder>/amity-provisioned.bin
```

If the `esptool` command is unavailable, use `python3 -m esptool` on macOS/Linux, or `python -m esptool` on Windows, in its place.

**Installing this full image replaces all existing settings and pairings.** Use it for initial installation only. Later browser updates use a different file, `amity-update.bin`.

Installation is now done, and Amity can now be setup for use in your home theater.

## Setup

### HDMI Connections

Enable HDMI-CEC on all the home theater system components. Manufacturers use different names for this setting; check your equipment's setup menus.

#### Single Adapter Amity

The single adapter Amity connects between the TV and the receiver.

1. Disconnect the HDMI cable connecting the TV's input to the receiver's output.
2. Connect the TV's HDMI input to the Amity HDMI port marked `Display`.
3. Use a second HDMI cable to connect the receiver's HDMI output to the Amity HDMI port marked `Source`.
4. Leave the playback devices connected to the receiver's inputs.

#### Multi-Adapter Amity

There are many options for connecting a multi-adapter Amity. Generally, there will be at least one between the display and other components, whether with a receiver or without. In addition an adapter can be connected between a receiver and a component to isolate a misbehaving component. In all cases, the 'Display' side of an adapter should be connected towards the display, and the 'Source' side of an adapter should be connected towards the A/V source.

### Matter and WiFi enrollment

Amity must be enrolled with a Matter home automation system to operate and to join your WiFi network.

Amity Matter setup opens automatically at boot on a fresh board with no current Matter pairings; no button press is required. An Amity whose last Matter pairing was removed opens Matter setup again at boot.

Use the Matter pairing information from the provisioned setup card to complete Matter enrollment.

Once Amity is enrolled with Matter, it will join your WiFi network and configuration can continue on Amity's admin page.

Note: HomeKit (Apple Home) successfully enrolls Amity but the Home app will display the Amity accessory as `Not Supported`. HomeKit does not support Matter Media Media Playback accessories directly. The workaround for Amity is to add it again to HomeKit as a HomeKit native accessory. See [HomeKit](#homekit).

## Administering Amity

In your web browser open the Amity administration page. For example, if the setup card hostname `amity-1234`, then browse to `https://amity-1234.local` (on Mac) or `https://amity-1234` (on Windows). The web browser will prompt that the site may be unsafe. This is because Amity creates a self signed security certificate for encryption. It is safe. Click on the details, and accept that the site is safe. Enter the admin password provided in the setup card. If Amity was provisioned without an initial password, create one when prompted.

### Activity Configuration

An activity describes a use of your home theater, such as 'Watch Apple TV' or 'Play PlayStation'. It specifies a display, a source, and an audio device. Selecting the activity turns on the appropriate equipment and routes the remote's commands to those devices.

Ensure that HDMI-CEC is enabled on all equipment, select the 'Activities' tab, and press 'Scan HDMI'. On a new installation, Amity creates a suggested list of activities from the devices it finds. Review the names and device selections, then press 'Save activities'.

Devices are identified by their HDMI On Screen Display (OSD) names, owning Amity, adapter and side. Devices on the same side of an adapter must have distinct OSD names.

If a device is missing, check its HDMI-CEC setting and connections, then scan again. Some devices only appear while powered on. A source that does not reliably appear can sometimes be used through [receiver input selection](#non-cec-sources).

### Editing the List of Activities

Drag activities to change their order. The position in the list determines which remote shortcut starts the activity; see [Using Amity](#using-amity).

Use 'Add activity' to create an activity, 'Edit' to change one, and 'Delete' to remove one. Press 'Save activities' when finished. 'Discard changes' returns to the saved configuration.

### Editing an Activity

The fields of an activity are:

* `Name` — a descriptive name of your choice.
* `Display` — the screen, typically your TV.
* `Source` — the playback device, such as a streamer or game console.
* `Audio` — the device that receives volume and mute commands, typically your receiver.
* `Switch Device` — an optional receiver or switch used for explicit input selection.
* `Input` — the switch device input number, shown only when a switch device is specified. See [Non-CEC Sources](#non-cec-sources).

To specify equipment without discovery, type its OSD name and choose 'Add component...'; select its Amity, adapter and side, then add it.

Peess 'Save activities' to save the activity changes.

#### Strange Devices

HDMI-CEC devices can behave strangely. Here are some known workarounds.

##### Nintendo Switch

The Nintendo switch has a minimal HDMI-CEC implementation that appears to be nearly always dormant and ignores practically every command except standby. The Switch only appears when it is powered on directly by the user, or placed in the cradle, and then it automatically requests to be displayed.

As a result, the Switch will often not show up in device scans, or be auto-configured. The solution is to create a new activity for the Nintendo Switch:

1. Press the '+' button to create a new activity.
2. Write in the activity name, for example `Play Switch`
3. Select the appropriate Display, and Audio devices.
4. Enter `NintendoSwitch` in the `Source` device field.
5. Press 'OK', and then press 'Save'.

Then to play, turn on the Switch, and select the `Play Switch` activity on the remote. Amity will do the right thing when the Switch is woken up, and announces itself. To end the activity, press the power button on the Siri remote to place the entire system in standby, including the Switch, which will go dormant again.

Sometimes, however, this may not be enough, and the Switch may still not announce its presence. For this case, the Switch can be configured similarly to a [non-CEC source](#non-cec-sources).

##### Non-CEC Sources

Non-CEC and Non-HDMI sources can be minimally supported, if the receiver supports input selection over HDMI-CEC. An activity for a non-CEC source can still control the display and receiver, including power and volume, but cannot control the power of the non-CEC source or send it menu commands. This limited support is particularly useful for old game consoles that require the use of their own game controllers in any case so a universal remote to control them is of limited value.

To support a non-HDMI source, Amity can be configured to select the input on the receiver. In the activity settings:

1. Select the receiver as the `Switch Device`.
2. Specify the `Input` number.

This functionality depends on the receiver supporting the feature, and on knowing the correct value for the input. Often input numbers are listed in the receiver's setup menus.

### Pairing a Remote

Charge the remote, and unpair it from any other device before pairing it with Amity.

1. Select the 'Remote' tab, and press 'Pair remote'.
2. Hold the remote close to Amity.
3. Start pairing on the remote:

   | Remote | Pairing buttons |
   |---|---|
   | Couch Clicker Atomic | Hold **Power + Volume Up** for six seconds to clear the previous bond and pair again. [Remote guide](https://github.com/retsyx/CouchClickerAtomic) |
   | Siri Remote, generation 2 or 3 | Hold **Back + Volume Up** for five seconds. [Apple's pairing instructions](https://support.apple.com/en-us/102569) |

4. Wait for Amity to report that the remote is paired.

Amity selects a nearby supported remote automatically. If pairing times out, move the remote closer and try again. To unpair the remote press 'Unpair remote'.

### Matter

The initial Matter pairing connects Amity to Wi-Fi and adds it to your home controller. Once activities are configured, the Matter controller can use Amity for home automation.

The matter tab lists controller connections. To add another controller, press 'Start pairing' and scan the displayed QR code in that controller's home app. The pairing window lasts five minutes.

Available controls may include power, activity selection, playback, volume, mute, and remote battery status. What appears depends on the home app and your equipment.

### HomeKit

Unfortunately, HomeKit does not support Matter Media Playback accessories directly. To be able to control Amity with HomeKit and Apple's Remote app, Amity must be added as a HomeKit native accessory.

In the HomeKit tab, press 'Start pairing', and scan the pairing QR code with the Apple Home app. The pairing window lasts five minutes.

HomeKit has its own pairing code, separate from Matter.

### Adding an Amity to a Multi-Amity Setup

To add an uninitialized Amity to an existing Amity system, open the Amity devices tab, and press 'Add'. Hold the new Amity’s `RESET` button for approximately three seconds. Enter the new Amity's name, and add it.

Note that Amity administration continues solely on the main Amity. Additional boards are configured and controlled entirely by the main Amity.

### Backup & Restore

Open the `Advanced` tab. In the `Configuration` section, press `Download` to save a portable backup, or select a backup to preview its import.

The backup contains activities and device-specific control settings. It does not contain Wi-Fi settings, admin credentials, pairings, hostname changes, or the HDMI-CEC address. Keep the original installation folder as well; a configuration backup does not replace your pairing card or USB installation image.

### Firmware Updates

Note: Downgrades are unsupported.

1. Download  `amity-firmware.zip` from the latest [Amity Release](https://github.com/retsyx/amity-esp32/releases/latest).
2. Unzip `amity-firmware.zip`. The update binary in is `amity-update.bin`.
3. In the admin page, open the `Advanced` tab and press the `Update firmware` button. Select the downloaded `amity-update.bin` file and confirm the installation.
4. Keep all Amities powered as the update progresses. Once the update is complete, all Amities will restart together.

### Factory Reset

In the `Avdvanced` tab, press the `Factory reset` button and confirm. This clears activities, Wi-Fi settings, Matter and HomeKit pairings, and the remote bond. It also restores the original hostname and initial admin password, if one was provisioned.

Alternatively, while Amity is powered, press the physical `RESET` button for 10 seconds to factory reset Amity.

## Using Amity

Amity has two basic modes: standby, when no activity is selected, and active, when an activity is running.

### Standby

A short press of the `power` button starts the first activity. Other activity shortcuts depend on your remote:

| Remote | Short press | Long press |
|---|---|---|
| Atomic activity buttons | Activities 1–4 | Activities 5–8 |
| Siri Remote Select, Up, Right, Down, Left | Activities 1–5, in that order | Activities 6–10, in that order |

These shortcuts select activities by their current order in the list.

A long press of the `power` button repeats the standby command. This is useful if a device is still on when it should be off.

### Active

During an activity, navigation and playback buttons control the source. Volume and mute control the audio device. For example, in a PlayStation activity, the directional buttons navigate the PlayStation interface, while the volume buttons control the receiver. Siri Remote touchpad swipes also navigate the source's menus.

A short press of the `power` button ends the activity and puts the system in standby. A long press repeats the current activity's setup. Use this if a device failed to turn on or selected the wrong input. The Activites tab `Repair` button in the browser performs the same action.

To change activities on Siri Remote, hold the `power` and press an activity-selection button. You can also select an activity in the browser.

Give your equipment a few seconds to finish switching before choosing a different activity. If Amity ignores the selection while the system is settling, press again after the transition finishes.

## License

Amity source code is licensed under the [GPLv3](https://github.com/retsyx/amity-esp32/blob/main/LICENSE). See the [third-party notices](https://github.com/retsyx/amity-esp32/blob/main/THIRD_PARTY.md) for dependency licenses.

