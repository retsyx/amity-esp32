# Isolated Raspberry Pi Matter controller

This fallback uses the official CHIP Python API from
`home-assistant-chip-core==2025.7.0` and its matching generated clusters. It was
tested with Raspberry Pi OS, Linux aarch64 and Python 3.13.5. The packages are
test tools; none are linked into the firmware. The CHIP packages use Apache-2.0.
Every wheel filename, version and SHA-256 is pinned in
[`matter_pi_requirements.txt`](matter_pi_requirements.txt), including the explicit
`aenum` dependency needed by this release.
The locked wheel metadata and bundled license files identify the Python
dependencies as Apache, BSD, MIT or ISC licensed. In particular, `ptyprocess`
reports an unspecified license in metadata but includes the ISC license text.
The installer retains the packages' license and notice files.

The existing harness installation remains beneath `/home/pi/amity/esp32`; moving the development repository does not relocate the Pi installation:

* `.matter-wheels`: downloaded, unmodified wheels.
* `.matter-python`: extracted Python modules and the relocated native SDK copy.
* `paa-root-certs`: public development attestation trust anchors.
* `test-results/matter`: private controller fabric keys, platform storage,
  credentials, logs and results. Keep this directory to test reboot and OTA.

## Reproduce the isolated dependencies

From the amity-esp32 repository root on the development host, download exactly the locked
wheels. These commands do not install anything into the host's Python environment.

```sh
mkdir -p .cache/matter-wheels .cache/pip .cache/tmp
TMPDIR="$PWD/.cache/tmp" python3 -m pip download \
  --cache-dir .cache/pip --dest .cache/matter-wheels \
  --only-binary=:all: --no-deps --require-hashes \
  --implementation cp --python-version 3.13 \
  --abi cp313 --abi abi3 --abi none \
  --platform manylinux_2_31_aarch64 --platform manylinux_2_28_aarch64 \
  --platform manylinux_2_17_aarch64 --platform manylinux2014_aarch64 \
  -r tests/matter_pi_requirements.txt
python3 tests/matter_pi_setup.py \
  --wheels .cache/matter-wheels --check-wheels-only
```

Copy the wheels, `matter_pi_setup.py`, `matter_pi_requirements.txt` and
`matter_pi_controller.py` into the same relative directories on the harness.
Copy only the public `.der` files from the installed SDK's
`connectedhomeip/connectedhomeip/credentials/development/paa-root-certs` into
`paa-root-certs`. On the Pi, from `/home/pi/amity/esp32`, run:

```sh
python3 tests/matter_pi_setup.py
```

The installer verifies all 36 exact wheel hashes before extraction. It refuses
to overwrite an existing installation. For the installation already present on
the test harness, use this read-only check instead:

```sh
python3 tests/matter_pi_setup.py --verify-installed
```

The packaged native library hardcodes three platform files under `/data`.
The installer changes only the equal-length, NUL-terminated storage strings
`/data/chip_factory.ini`, `/data/chip_config.ini` and `/data/chip_counters.ini`
to `/tmp//chip_*` in the isolated copy. It requires exactly one occurrence of
each original string, and refuses an unexpected binary. Protocol code is
unchanged. The original wheel remains hash-checked and unmodified.

## Run the real device checks

The credentials file must be mode 600 and contain `setup_pin` and `discriminator`
as integers, plus `ssid` and `password` strings when provisioning WiFi. The
existing approved harness WiFi credentials and DUT setup values are already in
`/home/pi/amity/esp32/test-results/matter/credentials.json`; do not print it.

Run the following on the Pi. `unshare` creates a private mount namespace, and
the test binds its own `platform` directory over `/tmp` only inside that
namespace. It leaves the harness's real `/tmp`, `/data` and system Python alone.
All SDK output must go into a private log because it can include credentials.

```sh
cd /home/pi/amity
umask 077
mkdir -p test-results/matter
sudo -n unshare --mount --propagation private \
  env PYTHONDONTWRITEBYTECODE=1 \
  PYTHONPATH=/home/pi/amity/esp32/.matter-python \
  python3 /home/pi/amity/esp32/tests/matter_pi_controller.py \
  --storage /home/pi/amity/esp32/test-results/matter \
  --trust-store /home/pi/amity/esp32/paa-root-certs \
  --credentials /home/pi/amity/esp32/test-results/matter/credentials.json \
  --isolate-platform-tmp --pase-only --expect-unknown-audio \
  > test-results/matter/controller.log 2>&1
sudo -n cat test-results/matter/result.json
```

This command checks clusters over BLE PASE. It does not provision WiFi or install
a fabric. `--expect-unknown-audio` is appropriate when the hub has not observed
an audio status report; omit it when testing an active audio system.
For a DUT already on WiFi, add `--pase-address ADDRESS` to use IP PASE during
an open commissioning window, without installing a test fabric.

For approved WiFi commissioning, omit `--pase-only`. For subsequent operational
CASE checks after reboot or OTA, omit both `--pase-only` and `--credentials` so the
controller reuses its existing fabric. `--exercise-controls` sends transport,
navigation, number and relative volume commands to the active activity; capture
the CEC bus to verify physical output.

`--expected-version VERSION` checks the Basic Information version after an OTA.
The root PartsList is checked against endpoints 1, 2 and 3 (Basic Video Player,
Speaker and remote battery). Endpoint 4 was removed in 0.1.44; Apple Home support
now uses the independent, optional HomeKit television described in
[HomeKit component notes](../components/amity_homekit/README.md).

`apple_fabrics_hardware.py` uses the same isolated CHIP installation and private
mount namespace to exercise fabric lifecycle callbacks through real Matter
commands. Its `observer`, `keychain`, `home1` and `home2` roles create separate
test fabrics; the Apple roles use vendor IDs 0x1384 and 0x1349 without involving
Apple services. `--action commission --address ADDRESS` needs an open
commissioning window and creates a fabric over the existing IP network without
reprovisioning WiFi. `--action remove --role observer --index INDEX` sends
Operational Credentials RemoveFabric, then confirms the observer remains
operational. `--action read` reuses a role's existing CASE credentials. Use fresh
private `--storage`, private `--credentials`, and the development `--trust-store`;
keep all native output in a private log. These are mutating hardware tests:
record test indexes and remove all temporary fabrics when finished. Removing
the last test Home also removes any pre-existing Apple Keychain orphan, so use
that scenario only when its cleanup is intended.

`--exercise-power-inputs` selects each configured activity and restores the
original selection and power state. For CEC audio feedback fixtures,
`--expect-volume 127 --expect-muted yes` checks the observed Matter level and
mute state. Matter levels range from 0 to 254; CEC levels range from 0 to 100.
`--speaker-mute muted` (or `unmuted`) sends one explicit mute command without
assuming that command acceptance proves a physical device changed state.
`--volume-step 20 --command-marker PATH` writes a private acknowledgment timestamp
for a synchronized CEC capture; the marker must not already exist.

`--replace-network` explicitly removes a differing staged WiFi network under the
commissioning failsafe before full commissioning. `--switch-network FILE` instead
uses an existing fabric to move to the private file's `ssid` and `password`,
committing only through `CommissioningComplete` after CASE reconnection. These
options change device network configuration and are not needed for read tests.
Temporary AP provisioning and restoration require the user's authorization.

For the authorized Pi AP fixture, `matter_pi_handoff.py` handles a radio limitation
observed on this harness: BLE establishment fails while the Pi's 2.4 GHz AP is
active. Start the AP fixture with its `--wait-file` option first, using a fresh
trigger path. Run the wrapper as root with `--storage`, `--credentials`,
`--trust-store`, the same `--ap-trigger`, a fresh alphanumeric `--attempt`, and
`--expected-version`. It releases the AP after the native ConnectNetwork send,
extends the existing PASE failsafe to 300 seconds after a successful response,
then immediately starts a fresh CASE controller for CommissioningComplete. The
AP helper runs separately and retains its timed restoration. The wrapper makes
no direct change to Pi network settings; private logs stay under `--storage`.
`--complete-staged` on the main controller is also available for this bounded
CASE completion recovery and requires the original controller fabric storage.

Hardware evidence includes successful BLE PASE cluster tests in
`test-results/matter/pi-pase-result.json` and completed WiFi/CASE commissioning in
`test-results/matter/pi-shared-mdns-result.json` on the development host. The
latter records firmware `0.1.2`, `fabric_count: 1`, successful CASE
CommissioningComplete and the real endpoint/command assertions. These private
artifacts contain test setup details and remain outside version control.
