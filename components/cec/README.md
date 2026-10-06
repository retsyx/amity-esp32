`cec::Engine` is a standalone, allocation-free CEC wire state machine. It has
no knowledge of Amity, device types, opcodes, or GPIO numbers. `cec::Bus` adds
the ESP-IDF GPIO/GPTimer and FreeRTOS queue bindings. Its GPIO is always open
drain; the board supplies the CEC pull-up. Each bus uses one GPTimer.

The timer samples every 50 microseconds. Start bits are 3.7 ms low / 4.5 ms
total. Data bits are 0.6 ms or 1.5 ms low / 2.4 ms total. The engine samples at
1.05 ms, acknowledges its allocated address, leaves broadcast ACK high,
detects arbitration loss, validates receive pulse widths, and abandons a
transmission when an ISR delay exceeds 200 microseconds. Direct and broadcast
acknowledgements have opposite polarity. Frames are bounded to 16 bytes.
These protocol timing values can also be checked against the
[Linux CEC pin driver's timing definitions](https://github.com/torvalds/linux/blob/master/drivers/media/cec/core/cec-pin.c).
This component is an original implementation, not a copy of that driver.

Transmit calls serialize with the per-bus worker; `send()` queues a frame
with an optional bounded capacity wait for producer backpressure,
while `transmit()` waits for a result and performs bounded retries. A one-byte
poll can be sent with one attempt for logical-address allocation/discovery.
`send_and_wait()` joins the same FIFO and waits for completion. Held keys use
one wire attempt; activity setup can request the ordinary three attempts.
Held-key callers can then check whether the button is still down before queuing
the next press; it does not add a repeat interval or bypass queued releases.
An absent device's polling NACK does not increment the error counter.
The receiver remains running during transmission and follows the winning
initiator after arbitration loss. Ordinary messages use a conservative seven
bit-period idle interval; retries use three.

Enable `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
`CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM`. All wire paths and their literal pools
reside in IRAM, and ISR state and queues are allocated in internal RAM. GPIO
access uses ESP-IDF's GPIO LL functions so cache-disabled flash/OTA operations
do not send the ISR through flash-resident functions.

Run `sh tests/cec_hub_test.sh` from the repository root for host wire
simulation under ASan/UBSan. After flashing the complete firmware,
`python3 tests/cec_hardware.py --host pi@10.0.1.119` uses the Linux GPIO
fixture to identify the front/back mapping and test both buses concurrently.
The hardware test intentionally leaves `HarnessTV` and `HarnessSource`
configured for web discovery and activity tests. `--inspect` is read-only.
