Upstream: https://github.com/espressif/esp-homekit-sdk
Commit: 676fabac4a4a05184be020611cb069faa0016411

Only HAP core, platform, standard profiles, HKDF and SRP components are vendored.
Source license headers, mu_srp/LICENSE and bundled RFC notices are retained.

Local adaptations:
- ESP-IDF 6.1 / Mbed TLS 4 private MPI compatibility and libsodium RNG interface.
- HAP SRP-3072 padding, proof validation and key agreement error handling.
- Twelve-byte AEAD nonces, per-session record buffers/counters, bounded records,
  complete sends/reads, authentication error handling and session data clearing.
- Use Matter's existing mDNS responder with Amity's delegated hostname.
- Explicit application-owned pairing windows checked at every setup phase;
  reopening invalidates in-flight sessions from previous windows.
- Recursive mutex serializes worker, event loop, HTTP, notification, timer and
  secure-session cleanup access. Setup timeout validates the live context.
- HAP allocations use PSRAM; task stacks stay internal.
- Removing HomeKit pairings targets only hap_ctrl; whole-partition erase disabled.

The surrounding installed ESP-IDF and ESP-Matter trees are not modified.
