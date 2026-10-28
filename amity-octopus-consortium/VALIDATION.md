# Current contract validation

The current package describes qualified saved intent and original owner-local execution. Removed planner/resource/binding/route approval fixtures are not active alternatives. The 27 October 2026 instruction and migration lesson are recorded in [AGENTS.md](../AGENTS.md).

## Checks and limits

`validate_contracts.py` checks schema-3 saved intent, qualified selectors and separate runtime endpoints, current scoped owner intents, supported management payload references, rejection of obsolete graph/kinds/fields, actual wire-kind registry consistency, hardware-capacity agreement, local documentation links and exact manifest coverage/hashes. Its hardware-capacity-only mode uses the standard library; full validation uses jsonschema/referencing and performs no network/device operations.

Run:

```sh
python3 amity-octopus-consortium/validate_contracts.py --hardware-capacity-only
python3 amity-octopus-consortium/validate_contracts.py
```

A contract fixture passing does not establish firmware execution. Current software and native-build commands/results are recorded in `IMPLEMENTATION_STATUS.json` after they run. Compare baseline local, qualified all-local and remote-owner transcripts for AVR/audio/switch/quirks, missing-name return, original discovery/PA, key lifecycle and FIFO8. A Ready endpoint, build or retained JSON alone is insufficient.

## Direct admission revision — 27 October 2026

The current contracts pass with `.cache/contracts-python/bin/python`: 34 manifest files, 6 schemas, 17 positive and 59 negative vectors, 4 configured selectors, 19 current wire kinds and 119 local links. Retired relay kinds are rejected.

The full host runner exited 0 in `test-results/admission-2026-10-27/host-tests.log`, including 29 independent passive-harness tests. Updated harness checks use normal ephemeral encrypted admission, fragmented GATT confirmation, exact scan/token selection and retained TLS/removal cases; no separate Add code is produced. Actual API and UI suites also passed (`admission-api-tests.log` and `admission-ui-tests.log` in that directory). The normal native build passed in `build-normal.log`; final strict-build and focused crypto outcomes are tracked in `IMPLEMENTATION_STATUS.json` after execution.

No actual ESP32 radio admission or new physical deployment is claimed. The firmware SRP self-test is compiled but has not run on an ESP32 in this task. The earlier refactor evidence below remains separate from this admission revision.

## Previous qualified-lookup refactor — 27 October 2026

The full validator passed using `.cache/contracts-python/bin/python`: 34 manifest files, 6 schemas, 17 positive and 55 negative vectors, 4 configured selectors, 23 current wire kinds and 119 local links. Hardware-capacity-only validation also passed for four adapters/eight CEC ports. These are contract/software checks; they perform no firmware/device operations. Firmware tests/build results are recorded separately after execution.

The actual synchronization integration suite `sh tests/system_sync_test.sh` also passed with ASan/UBSan: minimal identity/revision handshake and first-lease nonemitting context hook, failed snapshot IPC admission, derived control/realtime/cleanup authority with unchanged receiver auth/deadline, deferred owner-local lookup results bound to the exact authenticated link, actual source presence despite CEC admission failure, no fabricated lookup after IPC failure, setup completion/pending-reply snapshot metadata, exact boot/link disconnect, callback cleanup on failed initialization, sparse scans, saved adapter capacity retention, applied-revision status and real journal commit/failure independent of offline peers. Its SystemControl boundary is a spy; actual owner/facade CEC behavior is covered separately.

The full host runner `sh tests/run_host_tests.sh` exited 0 using pinned SDK Python 3.14 plus the local contracts site-packages. Its log is `test-results/host/intent-full-suite.log`; coverage includes the retained CEC/owner/configuration/remote/protocol/security/update suites, 27 local hardware-harness tests and offline production-security profile. After the final duplicate-name lookup simplification, `sh tests/cec_hub_test.sh` passed all five baseline suites and `sh tests/owned_hub_test.sh` passed its main and nonzero-settling suites. Those final checks cover baseline first OSD match/side isolation, coordinator-cache independence, two-owner AVR/switch/quirks, startup taps, observation IPC pressure and global last-completion settling.

For the earlier 0.1.81 refactor, `idf.py build provisioning-package` passed with the pinned SDK environment, including 35 public-record layout checks and package generation. The application is 2,420,848 bytes (`0x24f070`) in the `0x3d0000` OTA slot, with `0x180f90` free (39%). Image SHA-256 is `f7a28b2ff519eacc32e3a7ed3550eff2d39d9c27f7c21cc0cad6e54fe306e5af`; the log is `build/qualified-lookup-build.log`. The final build includes the baseline first-name lookup correction. This task has not deployed that image to a device.

## Historical evidence

The earlier graph-model schema/vector counts and firmware checks belong to their original commits/artifacts. The 0.1.80 deployment verified two image/startup results and independent credentials; its retained activity metadata concealed disabled AVR/switch behavior. It is not parity evidence for this model. Historical details remain in git and existing test-results artifacts instead of being retained as a second active design.

## Physical scope

No physical radio, CEC, AV, four-adapter timing, ecosystem retention or production-security result is inferred from documentation/host tests. Record actual device identity, image/source, configuration, input/observations and wire captures separately when such work is explicitly performed.
