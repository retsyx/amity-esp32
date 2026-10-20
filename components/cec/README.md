> 20 October 2026 scope amendment: the original first-setup Amity alone owns the sole remote, UI/admin and home integrations. Added devices are UI-less actuators, with no member input gateway or coordinator promotion/handover. Preserve the original activity UI with only canonical Amity-hostname context and one Amity management tab. [Normative contract](../../amity-octopus-consortium/21-original-controller-policy.md).

`cec::Engine` is a standalone, allocation-free CEC wire state machine. It has
no knowledge of Amity, device types, opcodes, or GPIO numbers. `cec::Bus` adds
the ESP-IDF GPIO/GPTimer and FreeRTOS queue bindings. Its GPIO is always open
drain; the board supplies the CEC pull-up. All active buses share one GPTimer while retaining independent wire engines.

The timer samples every 50 microseconds. Start bits are 3.7 ms low / 4.5 ms
total. Data bits are 0.6 ms or 1.5 ms low / 2.4 ms total. The engine samples at
1.05 ms, acknowledges its allocated address, leaves broadcast ACK high,
detects arbitration loss, validates receive pulse widths, and abandons a
transmission when an ISR delay exceeds 200 microseconds. Direct and broadcast
acknowledgements have opposite polarity. Frames are bounded to 16 bytes.
These protocol timing values can also be checked against the
[Linux CEC pin driver's timing definitions](https://github.com/torvalds/linux/blob/master/drivers/media/cec/core/cec-pin.c).
This component is an original implementation, not a copy of that driver.

Legacy transmit calls serialize with their per-bus worker. Native multiport buses use one bounded nonblocking dispatcher with independent queues and in-flight state; `send()` queues a frame
with an optional bounded capacity wait for producer backpressure,
while `transmit()` waits for a result and performs the caller's bounded attempts. A one-byte
poll can be sent with one attempt for logical-address allocation/discovery.
`send_and_wait()` joins the same FIFO and waits for completion. Every profile
has one TX FIFO per wire, with eight normal slots plus one release-only overflow
slot. This is the only
outgoing-frame buffer; the executor submits directly without another queue.
Finite batches are admitted entirely or return busy without enqueuing a prefix.
Ordinary submissions reject at eight queued-plus-active messages. An explicit
caller overflow flag permits one message in slot nine; the executor supplies it
only for release. At nine, further releases are dropped without an executor
admission retry. CEC applies the flag mechanically without inspecting opcodes.

CEC has no button state, key retry policy, repeat loop or release authority.
It sends the exact supplied frames with supplied attempt budgets and generic
deadline/generation guards, then reports outcomes. It must not inspect an opcode
to select a retry budget or create a semantic message. The executor supplies
one attempt for every key-press frame and owns completion-paced repetition.
Under the user's explicit amendment, failed key TX ends that key's pressed state
and repetition; a later button-up is a no-op. The executor selects implicit
release at recipient stream end, including last-key failure, only when all its
keys cease being pressed. CEC only buffers and transmits that supplied release;
it never derives this decision from an opcode or completion. Bounded completion and guard metadata is
retained through terminal completion, including after cancellation.
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
