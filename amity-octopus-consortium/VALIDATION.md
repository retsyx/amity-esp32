# Current contract validation

The current package describes qualified saved intent and original owner-local execution. Removed planner/resource/binding/route approval fixtures are not active alternatives. The 27 October 2026 instruction and migration lesson are recorded in [AGENTS.md](../AGENTS.md).

## Checks and limits

`validate_contracts.py` checks schema-3 saved intent, qualified selectors and separate runtime endpoints, current scoped owner intents, supported management payload references, rejection of obsolete graph/kinds/fields, actual wire-kind registry consistency, hardware-capacity agreement, local documentation links and exact manifest coverage/hashes. Its hardware-capacity-only mode uses the standard library; full validation uses jsonschema/referencing and performs no network/device operations.

Run:

```sh
python3 amity-octopus-consortium/validate_contracts.py --hardware-capacity-only
python3 amity-octopus-consortium/validate_contracts.py
```

A contract fixture passing does not establish firmware execution. Current software and native-build commands/results are recorded in `IMPLEMENTATION_STATUS.json` after they run. Compare baseline local, qualified all-local and remote-owner transcripts for AVR/audio/switch, missing-name return, original discovery/PA, key lifecycle and FIFO8. A Ready endpoint, build or retained JSON alone is insufficient.

## Audit fixes — 28 October 2026

Firmware `0.1.88` fixes four audited cases: confirmed radio cleanup after a timeout
returns to ordinary operation without reviving expired setup; a rejected local
key-down leaves no token that can swallow a press after adapter return; Add and
reset use one accepted physical press-edge clock; scan evidence validates the
actual OSD-name and physical-address/device-type reply bytes. The owning activity,
membership, radio and functional-test documents record those contracts.

`sh tests/run_host_tests.sh` exited 0, including the new radio-worker, qualified
local-key/reconnect and GPIO gesture regressions and all 165 harness unit tests.
The log is `test-results/audit-fixes-2026-10-28/host-tests.log`. Independent focused
CEC/owner, consortium, radio and harness suites also passed. The new scan negatives
reject matching cached UI state paired with rewritten identity replies.

The pinned SDK `idf.py build provisioning-package` passed for the existing
four-adapter profile, including all 35 public-record layout checks. This build
also includes the separately pending reference and 12 KiB task-stack changes;
its image hashes describe that combined working tree. The image is
2,418,752 bytes (`0x24e840`), leaving 39% of its OTA slot free; its SHA-256 is
`3054b98afccb451c2926e29088e7297775a8293b915f82ae1a9319b4161e5688` and ELF identity is
`3c17f9bfc32283ea4812501040bf9091fd360c9ae8b1be0238524f8695aa6cc8`.
Build evidence is `test-results/audit-fixes-2026-10-28/build.log`. This image has not
been deployed, and the strengthened scan assertions have not run on the physical
bench. Earlier physical results remain separate evidence.

## amity-dut update and reset — 29 October 2026

The subsequent build from committed source `2d148ca` passed with the pinned SDK
and was installed on `amity-dut` through the existing authenticated firmware
endpoint. Version `0.1.88` and ELF identity
`c9849838205bbf60f2558603f44d035d896a33f4b714bf062bb17c4878b48c2e` booted
successfully. The saved `0.1.86` configuration still contained the removed empty
`quirks` field, which the new reader rejects; the initial boot therefore retained
storage with activity control passive. A healthy image alone did not establish
successful configuration startup.

The user authorized a factory reset. The existing harness helper parsed the
documented reset wiring, held only this DUT's FW RESET for 10.2 seconds, and
released it to input with pulls disabled. The user then confirmed Matter
re-enrollment. Authenticated verification confirmed the same stable device ID,
the expected version/ELF, ready original-main authority and UI, working factory
initial password, two Matter fabrics and an empty new configuration. Boot ID is
`85adbb9ed72b69033230ba8784237cb0`. Factory credentials were retained; old
configuration and pairings were explicitly reset. The user restored hostname
`amity-dut`. Read-only checks after that restart verified boot
`adf4abfb5b747b0b0cf5ef5aa03e393f`, the same expected image, ready main/UI,
unchanged fresh configuration, working factory password, two fabrics and a closed
commissioning window.

Private evidence is in `test-results/audit-deployment-0.1.88/`, including
`build.log`, `deployment.json`, `new-boot-diagnostics.json`,
`factory-reset-action.json`, `factory-reset-uart.log`, `after-reset.json` and
`after-hostname.json`.
No other DUT was updated. This verifies image, reset and main/UI readiness;
physical CEC/AV/key behavior, multi-adapter timing and Amity admission remain
separate qualification work.

Current credential records are consolidated in the ignored private
`test-results/credentials/` directory. Cleanup removed 89 older or duplicate
credential carriers and redacted superseded credentials in 13 historical
evidence files. The four current records and latest original provisioner cards
were preserved; no setup-card QR, password image or instructions were changed.

## Source theft functional test — 29 October 2026

The new `source_theft.py` configures a saved activity for each of two playback
devices behind one AVR, starts the first, and makes the second claim Active
Source. All three functional scenarios pass on `test2` against `amity-dut`
0.1.88; all 204 framework unit tests also pass. The second playback model uses
the tuner allocation pool, retaining playback identity and behavior, so the
shared back wire can claim 5/4/3 in CEC 1.4 mode. The test observes actual DUT
directed Standby and Set Stream Path frames, verifies the original activity
and boot, and removes both temporary activities. Saved activities/equipment
are preserved and all 14 GPIO address masks are clear after cleanup. Reports
are `test-results/functional/source-theft-suite-0.1.88.json` and
`source-theft-verified.json`; host output is `source-theft-framework-tests.log`
in the same directory. No firmware API or behavior changes were made. This
qualifies the exercised local CEC path with simulated components, not real AV
behavior, all adapters or remote radio delivery.

## Standby stack fix — 28 October 2026

The activity functional test committed at `c4833a1` exposed a firmware failure on `amity-dut` running 0.1.85: start completed and the TV, AVR and player each received a real DUT power-on command, then standby caused an `amity_hub` task stack overflow and software CPU reset. The original failure report is `test-results/functional/activity-suite-0.1.85.json`; the detected overflow is in `test-results/functional/activity-dut-uart.log`. Target-compiled stack analysis found a standby call-chain subtotal above 6,304 bytes before downstream owner-send/runtime overhead, exceeding the 6,144-byte allocation. No recursive callback cycle was found.

Firmware 0.1.86 increases only that shared task stack from 6,144 to 12,288 bytes. The additional internal-RAM allocation is 6,144 bytes once, including on the unchanged four-tentacle profile. Activity planning, CEC handling, timing, attempt budgets and owner delivery are unchanged. Both `sh tests/cec_hub_test.sh` and `sh tests/owned_hub_test.sh` passed; logs are `test-results/functional/standby-stack-cec-host.log` and `test-results/functional/standby-stack-owned-host.log`. The pinned SDK build passed in `test-results/functional/standby-stack-build-0.1.86.log`, including all 35 public-record layout checks. The application uses `0x2528f0` bytes of its `0x3d0000` OTA slot, leaving 39% free.

Only `amity-dut` was updated to 0.1.86 through the existing authenticated application-update route. Its four-tentacle profile and existing configuration were preserved. The verified running ELF SHA-256 is `6cd6bb5195da40c2174aefc7d5b86993b5966cd556fc4e8f3a96844c34a32997`, with healthy boot ID `0c508459411bbdbf06b380cf41f7f46b`. The update evidence is `test-results/standby-stack-fix-0.1.86/after-update.json`. The earlier 0.1.84 four-board Matter boot/setup evidence remains in `IMPLEMENTATION_STATUS.json` and its original artifacts; this update makes no new deployment claim for the other three boards.

The complete HDMI scan/activity suite passed in `test-results/standby-stack-fix-0.1.86/functional-suite.json`. Two further activity cycles passed in `activity-cycle-2.json` and `activity-cycle-3.json` in the same directory. All three cycles kept the verified boot identity, observed real DUT-origin power-on commands and directed Standby `0x36` frames to all three simulated components, completed UI operations/settling, and removed only the temporary activity and its unreferenced fixture equipment. Final `verified.json` confirms the original activities and equipment were preserved; expected revisions and normal external-slot tombstones advanced. `adapters-after.json` confirms all 14 harness Linux CEC adapters released their claimed logical addresses after cleanup. These tests use existing authenticated UI endpoints and actual GPIO CEC traffic; virtual component traffic cannot satisfy the physical power assertions.

Current internal free heap was 32,283 bytes after update, 32,031 bytes after the first cycle and 32,015 bytes after all three cycles; the largest internal free block was 11,264 bytes at all three snapshots. See `after-update.json`, `after-cycle-1.json` and `verified.json` in the same directory. Remaining task-stack margin and post-workload historical minimum heap were not measured. Host workers run on the host stack, so their passing tests do not establish target stack capacity. The physical regression evidence is completion of the exercised start/standby paths, actual standby frames and an unchanged boot identity. Four-adapter/eight-wire electrical timing, ESP32 owner-to-owner radio delivery, real remote input and real AV behavior remain qualification gates.

## Direct admission revision — 27 October 2026

The current contracts pass with `.cache/contracts-python/bin/python`: 34 manifest files, 6 schemas, 17 positive and 59 negative vectors, 4 configured selectors, 19 current wire kinds and 119 local links. Retired relay kinds are rejected.

The full host runner exited 0 in `test-results/admission-2026-10-27/host-tests.log`, including 29 independent passive-harness tests. Updated harness checks use normal ephemeral encrypted admission, fragmented GATT confirmation, exact scan/token selection and retained TLS/removal cases; no separate Add code is produced. Actual API and UI suites also passed (`admission-api-tests.log` and `admission-ui-tests.log` in that directory). The normal native build passed in `build-normal.log`; final strict-build and focused crypto outcomes are tracked in `IMPLEMENTATION_STATUS.json` after execution.

No actual ESP32 radio admission or new physical deployment is claimed. The firmware SRP self-test is compiled but has not run on an ESP32 in this task. The earlier refactor evidence below remains separate from this admission revision.

## Previous qualified-lookup refactor — 27 October 2026

The full validator passed using `.cache/contracts-python/bin/python`: 34 manifest files, 6 schemas, 17 positive and 55 negative vectors, 4 configured selectors, 23 current wire kinds and 119 local links. Hardware-capacity-only validation also passed for four adapters/eight CEC ports. These are contract/software checks; they perform no firmware/device operations. Firmware tests/build results are recorded separately after execution.

The actual synchronization integration suite `sh tests/system_sync_test.sh` also passed with ASan/UBSan: minimal identity/revision handshake and first-lease nonemitting context hook, failed snapshot IPC admission, derived control/realtime/cleanup authority with unchanged receiver auth/deadline, deferred owner-local lookup results bound to the exact authenticated link, actual source presence despite CEC admission failure, no fabricated lookup after IPC failure, setup completion/pending-reply snapshot metadata, exact boot/link disconnect, callback cleanup on failed initialization, sparse scans, saved adapter capacity retention, applied-revision status and real journal commit/failure independent of offline peers. Its SystemControl boundary is a spy; actual owner/facade CEC behavior is covered separately.

The full host runner `sh tests/run_host_tests.sh` exited 0 using pinned SDK Python 3.14 plus the local contracts site-packages. Its log is `test-results/host/intent-full-suite.log`; coverage includes the retained CEC/owner/configuration/remote/protocol/security/update suites, 27 local hardware-harness tests and offline production-security profile. After the final duplicate-name lookup simplification, `sh tests/cec_hub_test.sh` passed all five baseline suites and `sh tests/owned_hub_test.sh` passed its main and nonzero-settling suites. Those final checks cover baseline first OSD match/side isolation, coordinator-cache independence, two-owner AVR/switch, startup taps, observation IPC pressure and global last-completion settling.

For the earlier 0.1.81 refactor, `idf.py build provisioning-package` passed with the pinned SDK environment, including 35 public-record layout checks and package generation. The application is 2,420,848 bytes (`0x24f070`) in the `0x3d0000` OTA slot, with `0x180f90` free (39%). Image SHA-256 is `f7a28b2ff519eacc32e3a7ed3550eff2d39d9c27f7c21cc0cad6e54fe306e5af`; the log is `build/qualified-lookup-build.log`. The final build includes the baseline first-name lookup correction. This task has not deployed that image to a device.

## Historical evidence

The earlier graph-model schema/vector counts and firmware checks belong to their original commits/artifacts. The 0.1.80 deployment verified two image/startup results and independent credentials; its retained activity metadata concealed disabled AVR/switch behavior. It is not parity evidence for this model. Historical details remain in git and existing test-results artifacts instead of being retained as a second active design.

## Physical scope

Documentation and host tests do not establish physical qualification. The local scan/activity result above is limited to amity-dut and one adapter with simulated HDMI-CEC equipment; it does not establish ESP32 radio delivery, real AV/remote behavior, four-adapter timing, ecosystem retention or production security. Record actual device identity, image/source, configuration, input/observations and wire captures separately for each physical result.
