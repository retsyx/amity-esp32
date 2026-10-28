# Building Amity locally

This guide prepares a macOS or Linux computer to build Amity firmware. Windows users can use Ubuntu 24.04 in WSL2: keep the checkout in the Linux home directory, then copy the generated image to Windows for flashing through its COM port.

The downloadable releases already contain firmware and a standalone setup tool. Local builds need Git, Bash, Python 3.11 or newer supported by the SDK, CMake 3.22 or newer, and Go 1.23 or newer. The first SDK checkout and firmware build can take a while and require Internet access.

## 1. Install prerequisites

### Ubuntu 24.04

Install Go 1.23 or newer using the [official Go installation instructions](https://go.dev/doc/install). Install the remaining dependencies:

```sh
sudo apt-get update
sudo apt-get install --yes --no-install-recommends \
  git wget flex bison gperf gcc g++ pkg-config cmake ninja-build ccache \
  curl unzip python3-venv python3-dev python3-pip libffi-dev libssl-dev \
  dfu-util libusb-1.0-0 libdbus-1-dev libglib2.0-dev libavahi-client-dev \
  libgirepository1.0-dev libcairo2-dev libreadline-dev libevent-dev default-jre
```

### macOS

Install the Xcode command-line tools and [Homebrew](https://brew.sh/), then:

```sh
xcode-select --install
brew install python@3.11 cmake ninja dfu-util ccache go
export PATH="$(brew --prefix python@3.11)/libexec/bin:$PATH"
```

Use the existing command-line tools if they are already installed. Confirm that `python3 --version` reports Python 3.11 or a newer version supported by the SDK, and that `go version` reports at least 1.23.

## 2. Get the Amity source

```sh
git clone https://github.com/retsyx/amity-esp32.git
cd amity-esp32
bash
```

To build a released version, run `git checkout RELEASE_TAG`, replacing `RELEASE_TAG` with the release’s tag, such as `v0.1.49`, before continuing. Otherwise, the build uses the checked-out development source.

Keep this Bash session open for the following steps. The setup commands below install SDKs inside the checkout’s `.cache` directory and use the exact versions recorded in its `SDK_VERSIONS.txt`.

## 3. Install the SDKs

```sh
export IDF_PATH="$PWD/.cache/esp-idf"
export ESP_MATTER_PATH="$PWD/.cache/esp-matter"
export IDF_TOOLS_PATH="$PWD/.cache/espressif"
mkdir -p .cache

AMITY_IDF_REF=$(awk '/^ESP-IDF / { print $NF }' SDK_VERSIONS.txt)
AMITY_MATTER_REF=$(awk '/^ESP-Matter:/ { print $NF }' SDK_VERSIONS.txt)
AMITY_CHIP_REF=$(awk '/^ConnectedHomeIP:/ { print $NF }' SDK_VERSIONS.txt)

git clone https://github.com/espressif/esp-idf.git "$IDF_PATH"
git -C "$IDF_PATH" checkout "$AMITY_IDF_REF"
git -C "$IDF_PATH" submodule update --init --recursive --depth 1 --jobs 4

git clone https://github.com/espressif/esp-matter.git "$ESP_MATTER_PATH"
git -C "$ESP_MATTER_PATH" checkout "$AMITY_MATTER_REF"
git -C "$ESP_MATTER_PATH" submodule update --init --depth 1 connectedhomeip/connectedhomeip
AMITY_CHIP_PATH="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip"
test "$(git -C "$AMITY_CHIP_PATH" rev-parse HEAD)" = "$AMITY_CHIP_REF"

AMITY_HOST_PLATFORM=linux
if [ "$(uname -s)" = Darwin ]; then AMITY_HOST_PLATFORM=darwin; fi
(cd "$AMITY_CHIP_PATH" && python3 scripts/checkout_submodules.py \
  --platform esp32 "$AMITY_HOST_PLATFORM" --shallow --jobs 4)

"$IDF_PATH/install.sh" esp32s3
source "$IDF_PATH/export.sh"
"$ESP_MATTER_PATH/install.sh" --no-host-tool
```

If you already have those exact SDK versions installed elsewhere, set `IDF_PATH` and `ESP_MATTER_PATH` to their locations instead of cloning them again. Set `IDF_TOOLS_PATH` to the matching installed tools directory as well, typically `~/.espressif`, or run the SDK installation steps for your chosen tools directory. SDK setup help is available in Espressif’s [ESP-IDF guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) and [ESP-Matter guide](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html).

## 4. Build the firmware and setup tool

From the Amity repository root in the same Bash session:

```sh
source tools/env.sh
idf.py set-target esp32s3
idf.py build provisioning-package
(cd amity-provision && CGO_ENABLED=0 go build -trimpath -o ../build/amity-provision-native .)
```

| Output | Purpose |
|---|---|
| `build/amity-firmware.zip` | Input to the setup tool for a fresh installation |
| `build/amity-update.bin` | One browser update file containing the release manifest, optional publisher signature and exact image |
| `build/amity.bin` | Raw application image for USB/debugging or an explicitly pre-staged update |
| `build/amity-provision-native` | Setup tool for the build computer |

The default board profile keeps the existing GPIO1/GPIO2 HDMI adapter mapping and GPIO4 setup/reset button. For a custom development carrier, run `idf.py menuconfig` and select **Amity board profile → Development carrier**. Its default isolated CEC GPIO assignments use the standard XIAO ESP32-S3 edge headers:

| HDMI adapter | Front GPIO | XIAO pin | Back GPIO | XIAO pin |
| --- | --- | --- | --- | --- |
| 1 | 1 | D0 | 2 | D1 |
| 2 | 5 | D4 | 6 | D5 |
| 3 | 7 | D8 | 8 | D9 |
| 4 | 9 | D10 | 44 | D7 / RX |

GPIO10 is not D10: the standard XIAO's D10 connects to GPIO9. These defaults leave GPIO4 / D3 available for the setup/reset button, avoid strapping GPIO3 / D2, and avoid GPIO43 / D6 / TX because ROM boot output can drive it before the firmware starts. GPIO44 / D7 / RX supports ordinary bidirectional GPIO; keep UART0 unused on that pin. The standard headers provide four adapter pairs under these constraints. [Seeed pinout](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/), [ESP32-S3 boot and UART behavior](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf).

Override these assignments in menuconfig to match the carrier, or set both GPIOs of an unused adapter to `-1`. Saved assignments, including explicit `-1` values, take precedence over these defaults. Adapter numbers stay fixed when connections are unused. Configure the optional active-low setup/reset button GPIO if the carrier needs the physical setup gesture; it defaults to disabled. A separate presence input is optional and defaults to unassigned; without one, physical attachment remains unknown and equipment availability comes from discovery. The CEC wires use external pull-ups with the internal GPIO pulls disabled.

The development profile accepts up to four adapters, including a device with none configured. Invalid mappings or allocation failures leave administration available with HDMI control disabled. Its GPIO mapping is included in the firmware identity, so an update for a different profile or mapping is rejected. Use a separate build directory for each carrier mapping and qualify its electrical behavior and simultaneous CEC timing before use.

To prepare a fresh device:

```sh
./build/amity-provision-native --hostname living-room --generate-password \
  --out living-room build/amity-firmware.zip
```

Continue with [USB installation and onboarding](README.md#install-over-usb). Flash the generated `living-room/amity-provisioned.bin` at address `0x0`; a plain `idf.py flash` does not prepare a fresh device’s pairing credentials. For later updates, upload `build/amity-update.bin` through **Advanced → Update Firmware**. Its embedded manifest and signature are verified before image installation; a production release must use the authorized signed container.

In a new Bash session, set `IDF_PATH`, `ESP_MATTER_PATH`, and `IDF_TOOLS_PATH` to the locations used during setup before sourcing `tools/env.sh`. Once an existing build is configured, use `idf.py build provisioning-package` for subsequent builds; `set-target` resets the build configuration.

## Upgrading an existing 0.1.80 installation to schema 3

Firmware using qualified owner lookup writes configuration schema 3. Its release
manifest must declare `config_write: 3` and readable configuration formats 1–3.
The storage journal wrapper remains storage format 2 with readable formats 1–2;
configuration schema and storage wrapper are separate compatibility axes.

The installed 0.1.80 update verifier accepts only manifests declaring configuration
writer 2, so it rejects a correctly labeled schema-3 update. Upgrade those boards
over USB using the matching hardware profile and verified target serial port:

```sh
source tools/env.sh
idf.py -p PORT flash
```

This application/bootloader/partition-table flash preserves the factory credential
and NVS partitions. Do not use a full `amity-provisioned.bin` installation image
for this upgrade: it replaces saved settings and pairings. The firmware reads
legacy v1/v2 configuration for migration, retaining original user intent/recovery
records until the new boot is confirmed. Do not mislabel schema-3 firmware as a
schema-2 writer or bypass the old verifier to make browser OTA accept it. Once
all relevant devices run the schema-3 firmware, ordinary compatible browser/member
updates use the correctly labeled schema-3 release container.

## Development and release checks

Firmware changes must increment `PROJECT_VER` and `PROJECT_VER_NUMBER` in `CMakeLists.txt`. Before 1.0, compatibility with prior Amity releases, provisioning records, and storage formats is not maintained. Keep the README and release notes clear about any installation requirements.

Run the local checks after SDK setup:

```sh
sh tests/run_host_tests.sh
sh tests/provisioning_test.sh
```

`python3 amity-provision/build_release.py` builds setup-tool archives for macOS, Windows, and Linux, in both Intel/AMD and ARM64 versions. The GitHub **Build firmware** workflow uses the same SDK pins. A newly pushed `v*` tag triggers **Release firmware**, which verifies that the tag’s version matches `PROJECT_VER` and publishes the firmware, setup-tool archives, installation README, and checksums as GitHub Release assets.

Technical references: [hardware pins](main/board_pins.h), [Matter](components/amity_matter/README.md), [HomeKit](components/amity_homekit/README.md), [remotes](components/ble_remote/README.md), [activity control](components/amity_hub/README.md), [device settings](components/device_settings/README.md), and [implementation and validation status](IMPLEMENTATION_STATUS.json).

## Original-controller application profile

One firmware serves both roles. A fresh device has no Wi-Fi credentials and hosts no UI. Completing Matter BLE enrollment establishes the original controller; adding a device over the authenticated Amity BLE channel selects its subordinate role. That durable choice survives removing Matter fabrics. Added devices retain the shared CHIP Wi-Fi/NimBLE substrate and authenticated peer config/OTA/status only; they host no UI or user-control integration and cannot promote themselves after reset or controller loss. No separate actuator-only build is required.

The 20 October 2026 amendment fixes the original device as sole controller/UI/admin/remote/Matter/HomeKit owner. Controller identity migration must be proven, never inferred from ordering; ordinary portable import cannot replace it within a live installation. Supported failed-main recovery instead Matter-enrolls replacement hardware as a new main, restores portable configuration through existing Advanced upload/restore, and explicitly re-pairs integrations/remote and re-enrolls members after old-main isolation. Preserve baseline UI commit `d50af8026938797fc0820d0107f4c4e064c2d2bc` with only canonical Amity-hostname context and one Amity management tab. Historical hardware/startup evidence is separate from the current qualified-lookup implementation. Current software checks and remaining physical qualification follow the [original-controller contract](amity-octopus-consortium/21-original-controller-policy.md) and [validation requirements](amity-octopus-consortium/15-validation-and-release-gates.md).

## Adding Amity devices

The default build adds a physically authorized unclaimed board through **Amity devices → Add** on the main. It uses ephemeral key exchange and encrypted messages; no separate Add code is generated or provisioned.

For installations that require code possession as well as the physical setup window, enable **Amity admission → Require the printed setup code when adding Amity** in `idf.py menuconfig` (`CONFIG_AMITY_ADMISSION_REQUIRE_SETUP_CODE=y`). Build both main and candidate with the same setting. A mismatch rejects admission without fallback. This mode accepts the candidate's existing printed Matter manual code or QR payload through PAKE to authenticate fresh Amity session keys; it does not perform Matter commissioning or create a fabric. Factory Matter/admin provisioning and the setup card are unchanged.
