Upstream: https://github.com/espressif/esp-homekit-sdk
Commit: 676fabac4a4a05184be020611cb069faa0016411

Only HAP core, platform, standard profiles and SRP components are vendored. The
hkdf-sha component implements the HKDF-SHA-512 and SHA-512 interface that pairing
and SRP use with PSA Crypto; the RFC 6234 code and its test driver are not vendored.
Source license headers and mu_srp/LICENSE are retained.

Local adaptations:
- ESP-IDF 6.1 / Mbed TLS 4 private MPI compatibility and libsodium RNG interface.
- HAP SRP-3072 padding, proof validation and key agreement error handling.
- Twelve-byte AEAD nonces, per-session record buffers/counters, bounded records,
  complete sends/reads, authentication error handling and session data clearing.
- Use Matter's existing mDNS responder with Amity's delegated hostname.
  Live hostname changes republish discovery while preserving sessions and pairings.
- Explicit application-owned pairing windows checked at every setup phase;
  reopening invalidates in-flight sessions from previous windows.
- Recursive mutex serializes worker, event loop, HTTP, notification, timer and
  secure-session cleanup access. Setup timeout validates the live context.
- Request bodies are capped and read with an absolute deadline before taking
  the HAP mutex. Pairing restarts publish replacement session contexts; pairing
  TLVs and characteristic identifiers are validated before use.
- HAP allocations and the HTTP task stack use PSRAM. Keystore operations use
  Amity's internal flash stack; the worker and event-loop stacks stay internal.
- Removing HomeKit pairings targets only hap_ctrl; whole-partition erase disabled.

The surrounding installed ESP-IDF and ESP-Matter trees are not modified.
