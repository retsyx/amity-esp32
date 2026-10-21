> 20 October 2026 scope amendment: the original first-setup Amity alone owns the sole remote, UI/admin and home integrations. Added devices are UI-less actuators, with no member input gateway or coordinator promotion/handover. Preserve the original activity UI with only canonical Amity-hostname context and one Amity management tab. [Normative contract](../../amity-octopus-consortium/21-original-controller-policy.md).

`cec::Engine` is a standalone, allocation-free CEC wire state machine. It has
no knowledge of Amity, device types, opcodes, or GPIO numbers. `cec::Bus` adds
the ESP-IDF GPIO/GPTimer and FreeRTOS queue bindings. Its GPIO is always open
drain; the board supplies the CEC pull-up. All active buses share one GPTimer
while retaining independent wire engines.

The timer samples every 50 microseconds. Start bits are 3.7 ms low / 4.5 ms
total. Data bits are 0.6 ms or 1.5 ms low / 2.4 ms total. The engine samples at
1.05 ms, acknowledges its allocated address, leaves broadcast ACK high,
detects arbitration loss, validates receive pulse widths, and abandons a
transmission when an ISR delay exceeds 200 microseconds. Direct and broadcast
acknowledgements have opposite polarity. Frames are bounded to 16 bytes.
These protocol timing values can also be checked against the
[Linux CEC pin driver's timing definitions](https://github.com/torvalds/linux/blob/master/drivers/media/cec/core/cec-pin.c).
This component is an original implementation, not a copy of that driver.

Local and distributed execution use the same `main` behavior. Distribution
only delivers messages to the owning wire; it must not change frames, order,
attempt budgets, admission or completion semantics. The APIs below provide
transport plumbing rather than alternate native/compatibility CEC policies.

Physical transmission is serialized within each wire's sole FIFO. All buses
use one bounded nonblocking dispatcher with independent queues and in-flight
state. `QueueProfile::Legacy` and `QueueProfile::Native` are compatibility
aliases and select identical CEC execution, capacity and receive behavior. `submit_guarded()` and `submit_guarded_batch()` admit work
immediately; `take_completion()` reports each operation token's terminal result
and whether a frame started. A finite batch is admitted entirely or returns
busy without enqueuing a prefix. All receive queues have 32 entries.

`send_async_guarded()` supplies the same submission/completion split with
caller-owned result storage. It attempts admission immediately and returns without
waiting for transmission or an ACK. Its caller-owned `TxCompletion` becomes
Pending on admission; the worker publishes a terminal result after releasing
FIFO capacity. Read the result with acquire ordering before reusing the
tracker, and keep both tracker and bus alive until completion. Rejection leaves
the tracker unchanged. Separate operations have separate completion trackers:
one pending ACK cannot block admission on its wire or progress on another wire.
This matches [Python Amity's CEC transmitter](https://github.com/retsyx/amity/blob/main/cec.py#L669-L683).

Runtime commands, held keys, activity setup, discovery and address allocation
submit without blocking the owner on CEC capacity or ACKs. Frames that do not
need completion tracking also use immediate admission. A one-byte poll can use
one attempt for logical-address allocation/discovery. Every profile has one TX
FIFO per wire, with eight normal slots plus one release-only overflow slot.
This is the only outgoing-frame buffer; the executor submits directly without
another frame queue.

Ordinary submissions reject at eight queued-plus-active messages. An explicit
caller overflow flag permits one message in slot nine; the executor supplies it
only for release. At nine, further releases are dropped without an executor
admission retry. CEC applies the flag mechanically without inspecting opcodes.

CEC has no button state, key retry policy, repeat loop or release authority.
It sends the exact supplied frames with supplied attempt budgets and generic
deadline/generation guards, then reports outcomes. It must not inspect an opcode
to select a retry budget or create a semantic message. The executor supplies
one attempt for every key-press frame and up to three for final release and
ordinary commands. Each held key has at most one outstanding attempt; its
completion controls only that key's continuation. No hub task waits for an ACK,
and there is no repeat timer or backlog of repeat frames. Failed key TX ends
only that key's pressed state. The executor supplies one final release when all
keys at that `(physical wire, initiator, destination)` have ended, including
last-key failure. CEC's `USER_CONTROL_RELEASED` has no key operand. Key-up does not cancel already
admitted frames or another key's completion. CEC only buffers and transmits the
supplied release; it never derives that decision from an opcode or completion.
Bounded completion and guard metadata is retained through terminal completion,
including after cancellation. Generic guards support exceptional invalidation;
a normal button-up does not advance a global generation guard.

An absent device's polling NACK does not increment the error counter.
The receiver remains running during transmission and follows the winning
initiator after arbitration loss. Ordinary messages use a conservative seven
bit-period idle interval; retries use three.

Enable `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
`CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM`. All wire paths and their literal pools
reside in IRAM, and ISR state and queues are allocated in internal RAM. GPIO
access uses ESP-IDF's GPIO LL functions so cache-disabled flash/OTA operations
do not send the ISR through flash-resident functions.

Run `sh tests/cec_bus_test.sh` and `sh tests/cec_hub_test.sh` from the repository
root for host simulation under ASan/UBSan. Also run
`sh tests/cec_runtime_test.sh` for the shared native runtime and completion-token
API, and `sh tests/system_hold_test.sh` for the executor policy.
`tests/cec_bus_test.cpp` exercises
immediate admission during an outstanding ACK, individual completion results,
FIFO capacity/reuse and independent buses. The hub tests cover independent
held keys, recipient release, failure cleanup and activity settling without
blocking command admission. After flashing the complete firmware,
`python3 tests/cec_hardware.py --host pi@10.0.1.119` uses the Linux GPIO
fixture to identify the front/back mapping and test both buses concurrently.
The hardware test intentionally leaves `HarnessTV` and `HarnessSource`
configured for web discovery and activity tests. `--inspect` is read-only.
