# Capturing firmware diagnostics

Use the original main's authenticated UI log download for capture that must
preserve its current boot:

```python
capture = dut.capture_logs(expected_boot=known_boot)
log_path.write_bytes(capture.data)
proof_path.write_text(json.dumps(capture.proof, indent=2) + "\n")
capture.assert_same_boot()
```

`DUT.capture_logs()` reads the existing `/api/system`, `/api/logs`, then
`/api/system` routes using the selected HTTPS verification and authenticated
session. It returns exact bytes and before/after device identity and boot
proof. `/api/logs` is plain text, so it must not be passed to the JSON-only
`DUT.request()` helper. The helper has one bounded deadline, never resends the
request and writes no files. Save logs privately under ignored `test-results/`;
raw logs are excluded from the capture object's repr and framework traces.

This is a snapshot of the firmware's existing bounded log buffer, not a
continuous stream. Capture promptly after a failure. Use it at diagnostic
boundaries, outside timed key actions and CEC assertions. Headless members do
not gain a separate HTTP UI or alternate diagnostic endpoint.

Do not reuse the previous pyserial recipe as passive capture. On 1 November
2026, opening it caused `USB_UART_CHIP_RESET` despite setting `dtr=False` and
`rts=False` before `open()`. Pyserial opens the OS device and then applies DTR
and RTS through separate calls; the OS driver may also change control lines
on open or close. ESP32-S3's native USB Serial/JTAG controller treats particular
DTR/RTS sequences as reset commands. Clearing DTR while RTS remains set can
therefore reset the board before the subsequent RTS clear.

The [ESP32-S3 technical reference manual](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf),
USB Serial/JTAG reset tables 33.4-3 and 33.4-4, describes those sequences.
Changing flags or suppressing pyserial's control writes alone cannot establish
that a specific host driver opens and closes without resetting. Serial capture
must have separate physical boot-preservation evidence before being described
as passive. The HTTP helper requires no serial device access.
