# Dependencies

Runtime dependencies are provided by ESP-IDF / ESP-Matter and their declared managed components. Consult the license files in each pinned SDK or downloaded component; the component lock pins the managed versions. The selected Espressif HomeKit components are vendored with their license headers and notices under `third_party/esp-homekit-sdk`; their exact upstream commit and local adaptations are recorded in `UPSTREAM.md`.

| Dependency | Source | License |
|---|---|---|
| ESP-IDF, NimBLE integration, HTTP(S), NVS, OTA | installed ESP-IDF | Apache-2.0; bundled third-party notices apply |
| Espressif HomeKit SDK core, platform and profiles | vendored at `676fabac4a4a05184be020611cb069faa0016411` | Espressif MIT |
| mu_srp | vendored with HomeKit SDK | Apache-2.0; LICENSE included |
| HKDF-SHA / RFC 6234 implementation | vendored with HomeKit SDK | BSD-style notices in source |
| libsodium | managed `espressif/libsodium` | ISC |
| Espressif JSON parser/generator | managed dependencies | Apache-2.0 |
| NimBLE | Apache Mynewt bundled in ESP-IDF | Apache-2.0 |
| ESP-Matter and CHIP | installed ESP-Matter / connectedhomeip | Apache-2.0 |
| Mbed TLS / TF-PSA-Crypto | bundled in ESP-IDF | Apache-2.0 OR GPL-2.0-or-later; Apache option used |
| Nayuki QR Code generator (C) | installed CHIP `examples/common/QRCode/repo/c` | MIT |
| cJSON | `espressif/cjson` | MIT |
| Espressif mDNS, secure certificate manager, encrypted image, delta OTA | managed SDK dependencies | Apache-2.0 |
| go-qrcode (standalone provisioning tool) | `github.com/skip2/go-qrcode` at `da1b6568686e` | MIT; license included in tool release archives |
| Go x/term 0.28.0, x/sys 0.29.0, x/crypto 0.31.0 (provisioning tool) | `golang.org/x` | BSD-3-Clause; notices included in release archives |
| Go x/image 0.23.0, x/text 0.21.0 and embedded Go fonts (provisioning setup card) | `golang.org/x/image`, `golang.org/x/text`; Go fonts by Bigelow & Holmes | BSD-3-Clause; code and font notices included in release archives |
| dbus-next (test fixture only) | PyPI 0.2.3 | MIT |
| Playwright (browser test only) | PyPI 1.56.0 | Apache-2.0 |
| Home Assistant Matter Server core / CHIP controller (Pi test only) | PyPI 2025.7.0 | Apache-2.0; bundled third-party notices apply |

The optional encrypted HomeKit test controller uses `aiohomekit==3.2.20` (Apache-2.0) and its dependencies in an isolated, ignored cache. It is not part of the firmware.
