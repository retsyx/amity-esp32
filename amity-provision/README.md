# amity-provision

Prepare Amity firmware and Matter pairing codes before flashing an ESP32.
The tool runs offline and does not require Go, Python, an ESP-IDF installation,
a serial console, or a connected device. Flashing is a separate step.

## Provision and install

Download `amity-firmware.zip` and the executable archive for your computer.
Extract the executable. Choose `darwin-arm64` for Apple Silicon,
`darwin-amd64` for Intel Macs, or the matching Windows/Linux architecture.
These development executables are unsigned and not notarized.

On macOS/Linux, run in a terminal:

```sh
./amity-provision --out living-room amity-firmware.zip
```

On Windows, run in PowerShell:

```powershell
.\amity-provision.exe --out living-room amity-firmware.zip
```

Options go before the firmware path. The tool generates a random Matter setup
PIN, discriminator and SPAKE2+ salt. It prints the manual pairing code and writes:

| File | Use |
|---|---|
| `amity-provisioned.bin` | Complete 8 MB fresh-install image; flash at **0x0** |
| `amity-factory.bin` | 4 KB factory partition; retained for recovery |
| `amity-pairing.png` | Setup card with QR, manual code, hostname and auto-generated password |
| `amity-pairing.txt` | Pairing details, hostname, auto-generated password and installation instructions |
| `amity-admin-password.txt` | Initial admin password, only when auto-generated |

Flash `living-room/amity-provisioned.bin` at address **0x0** using an ESP32
flasher configured for ESP32-S3, 8 MB flash, DIO, 80 MHz. With esptool:

```sh
esptool --chip esp32s3 --port PORT write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 living-room/amity-provisioned.bin
```

The full image replaces **all existing device configuration and pairings**.
Use it for fresh installation. For subsequent upgrades, extract `amity.bin`
from the firmware ZIP and upload it through **Advanced → Update Firmware**.
Never upload `amity-provisioned.bin` through the admin page.

`amity-pairing.png` is a printable setup card: the Matter QR code, its dashed
manual code directly below it, the hostname and administration URL, and the
auto-generated admin password when `--generate-password` is used. Passwords
supplied with `--password` or `--prompt-password` are omitted from the card and
pairing text. If no password was supplied, the card says “Not set.” Long generated
passwords wrap without adding characters. The same details are in
`amity-pairing.txt`.
These credentials describe the initially provisioned settings, not later UI changes.

Connect the external antenna, boot the device, and scan `amity-pairing.png`
with your Matter home app. Matter commissioning supplies the 2.4 GHz Wi-Fi
network. Then visit the administration URL printed by the tool (by default,
`https://amity.local`) and log in with the provisioned password, or set one if none was provisioned.
Uncommissioned devices advertise for the SDK's initial pairing window;
reboot to reopen it if it expires before setup.

Keep each device's output directory privately. Generate a separate directory
for each device; the tool refuses to overwrite one. These files contain setup
secrets. Unix outputs use owner-only permissions; on Windows use a private
user directory with appropriate access controls. Ordinary firmware upgrades
and Matter factory reset preserve the factory code; full flash erase does not.
Reflash the saved image to recover a fully erased device with its original code.

`--hostname` sets the network hostname and the `.local` administration address:

```sh
./amity-provision --out living-room --hostname living-room amity-firmware.zip
```

This device will be reachable at `https://living-room.local`. The default is
`amity`. Supply just the hostname, without `https://` or `.local`: 1–32 ASCII
letters, digits or hyphens, with no leading/trailing hyphen. Uppercase letters
are normalized to lowercase. The name is saved in factory data and survives
OTA updates and Matter factory reset. It sets the DHCP hostname, mDNS alias and
newly generated TLS certificate's name; existing TLS identities are retained.
Choose distinct names for devices on the same network.

An initial **admin password** is optional. Choose exactly one method:

```sh
# Hidden terminal entry, with confirmation:
./amity-provision --prompt-password --out living-room amity-firmware.zip

# Generate a random password and save it privately in amity-admin-password.txt:
./amity-provision --generate-password --out living-room amity-firmware.zip

# Supply it directly (the value can appear in shell history and process listings):
./amity-provision --password 'your chosen password' --out living-room amity-firmware.zip
```

Passwords accept 1–128 UTF-8 bytes, excluding NUL. Secure prompting requires an
interactive terminal, disables echo and restores terminal settings on cancellation.
Only a salted PBKDF2-HMAC-SHA256 hash is embedded in the firmware image.
Only auto-generated passwords appear in the setup-card PNG, pairing text and
`amity-admin-password.txt`. User-supplied and securely prompted passwords are
never included in these outputs; terminal output does not echo passwords. Auto-generated passwords are
12 characters long and contain 72 bits of randomness. With no password option, first access shows password onboarding.
The admin password is separate from the Matter setup PIN.

In **Advanced**, the user can change the hostname or password. Factory reset asks
for confirmation, clears activity configuration, Wi-Fi, Matter fabrics, remote
bonds, hostname overrides, password changes and generated TLS identity, then
restores the provisioned hostname and initial password. If no initial password
was supplied, onboarding returns. The original Matter setup code and installed
firmware remain. Add the device to your home again to provision Wi-Fi.
Hostname, credentials and Matter configuration are excluded from backup/restore;
configuration backups support only activities and vendor quirks.

`--passcode` optionally accepts a valid numeric Matter setup PIN (up to eight
digits), and `--discriminator` accepts 0–4095. The displayed **manual pairing
code is derived from both**, so it differs from the numeric setup PIN. Random
defaults are recommended. `--help` lists the options.

The package and tool currently support the XIAO ESP32-S3 with 8 MB flash and
8 MB octal PSRAM. They retain Amity's development attestation credentials;
provisioning a code does not create production Matter certification.

## Build and test

From this directory, with Go 1.23 or newer:

```sh
go test ./...
go build -trimpath -o amity-provision .
```

The QR dependency is MIT licensed; Go terminal, system and password-hashing
packages use BSD-3-Clause licenses. All are pinned in `go.mod`/`go.sum` and support
standalone builds without CGO.
Build six standalone release ZIPs with `python3 build_release.py` (maintainers
need Python and Go). Outputs are under `build/amity-provision/`, with
license notices and SHA-256 checksums. End users need only the executable.
macOS signing/notarization and Windows signing require release credentials
and are not performed by this script.

Build the matching firmware package from the repository root after sourcing `tools/env.sh`:

```sh
idf.py build provisioning-package
```

This creates `build/amity-firmware.zip`, containing bootloader, partition table,
initial OTA data, application and a versioned SHA-256 manifest. The provisioner
accepts only version-3 packages and validates the target, image bounds, checksums
and exact partition layout.
Checksums detect corruption; obtain both downloads from a trusted release source.

Run `sh tests/provisioning_test.sh` from the repository root for Go tests and the sanitized
C++ firmware parser tests. Both implementations share an independently encoded Python fixture at
`testdata/factory-v3.hex`.
The reset suite simulates interrupted writes throughout the reset procedure.

## Firmware contract

The `amity_factory` partition is an unencrypted data partition (subtype `0x40`)
at `0xf000`, length `0x1000`. All existing partition addresses are unchanged.
The first 288 bytes contain this little-endian version-3 record; the remainder
is erased (`0xff`):

| Offset | Length | Value |
|---|---|---|
| 0 | 8 | ASCII `AMITYPRV` |
| 8 | 2 | Record version: 3 |
| 10 | 2 | Record length: 288 |
| 12 | 2 | Matter vendor ID, from package |
| 14 | 2 | Matter product ID, from package |
| 16 | 4 | Valid Matter setup PIN |
| 20 | 2 | Long discriminator, 0–4095 |
| 22 | 2 | Reserved, zero |
| 24 | 4 | SPAKE2+ iteration count: 1000 |
| 28 | 32 | Cryptographically random salt |
| 60 | 4 | Reserved, zero |
| 64 | 64 | Hostname, 1–32 bytes followed by zero padding |
| 128 | 98 | Admin password record: 32 hex salt chars, `:`, 64 hex digest chars, NUL; all zero for onboarding |
| 226 | 30 | Reserved, zero |
| 256 | 32 | SHA-256 of bytes 0–255 |

The firmware and tool support only this record format. Password hashes use
PBKDF2-HMAC-SHA256, 10,000 iterations, a 32-byte digest and the ASCII hex salt
as the KDF salt.

Firmware validates the complete record before registering its commissionable
data provider. An invalid record stops Matter startup rather than silently
changing the code. Devices require valid factory provisioning data; an absent
or erased partition stops Matter startup. The firmware does not invent a code
at first boot.
The factory partition is outside both OTA slots and normal NVS reset storage.
