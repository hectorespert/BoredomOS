## Context

See `proposal.md` — Why. What follows is what the Renesas core actually does, since
three of the decisions below follow from it rather than from preference. All of it was
read from `framework-arduinorenesas-uno` as vendored by PlatformIO.

- `Serial1` is a macro for `_UART1_`, a concrete `UART` instance on `UART1_TX_PIN 1` /
  `UART1_RX_PIN 0` of the MINIMA variant. `Serial` is a macro for `SerialUSB`.
- `UART::operator bool()` is `return true;`, unconditionally. `_SerialUSB::operator
  bool()` reflects CDC connection state and calls `tud_task()` as a side effect.
- `UART::write(uint8_t*, size_t)` hands the buffer to the FSP driver and then
  **busy-waits** on a volatile flag until transmission completes. It does not buffer
  and return. `_SerialUSB::write()` returns `0` immediately when no host is connected.
- The UART receive path is interrupt-driven into a `SafeRingBufferN<512>`;
  `SERIAL_BUFFER_SIZE` is 512, not the 64 typical of AVR cores.
- `configUSE_TIME_SLICING` is `0` in this port's `FreeRTOSConfig.h`, and every option
  in that header is `#ifndef`-guarded, so `build_flags` can override them.

## Goals / Non-Goals

**Goals:**

- One definition names the link port, one names its baud rate, and both are
  overridable from `build_flags`.
- The move does not introduce a scheduling regression from the UART's busy-wait.
- Dead guards are removed rather than carried forward as no-ops.

**Non-Goals:**

- Deciding what the console is for. `Serial` is opened and left unused; the policy for
  diagnostics belongs to a later change.
- Touching `TaskSerialRead`'s polling loop. It is adequate here (see Decision 4) and
  its own backlog entry can be reconsidered on the evidence below.
- Any change to `configUSE_TIMERS`, task counts, stack sizes or queue depths.

## Decisions

### 1. The alias is a `#define`, not a `HardwareSerial&`

`include/Link.h` defines `LINK_SERIAL` and `LINK_BAUD`, each guarded by `#ifndef` so
`build_flags` wins.

The obvious alternative — `extern HardwareSerial& linkSerial;` in one translation unit
— is **wrong here for a concrete reason**, not a stylistic one. `UART` overloads
`write(uint8_t*, size_t)` as a **non-const** member and never overrides `Print`'s
virtual `write(const uint8_t*, size_t)`. Called through a `HardwareSerial&`, the
virtual const version is selected, whose base implementation loops calling
`write(uint8_t)` once per byte — turning one driver call and one busy-wait into 68 of
each for a `BATTERY_STATUS` frame. The macro keeps the concrete type at the call site
and selects the block-write overload.

The `#define` also gives the revert path the proposal promises: `-D LINK_SERIAL=Serial`
restores USB with no source edit, because `Serial` is itself a macro for a concrete
type with the same overload set.

### 2. `TaskSerialWrite` drops to `PRIORITY_HIGH`

`ARCHITECTURE.md` justifies `PRIORITY_HIGHEST` for serial I/O as *"bytes are lost if
the UART is not drained"*. That reasoning is about **reading**. Nothing is lost by
transmitting later: the frame waits in `serialWriteQueue`.

Meanwhile `UART::write` busy-waits, and with `configUSE_TIME_SLICING` at `0` a task
that neither blocks nor yields is not preempted by equals. At `PRIORITY_HIGHEST` a
single frame therefore stops the entire system for its transmission time:

| Frame | Bytes | @115200 | @57600 |
|---|---|---|---|
| `HEARTBEAT` | ~23 | 2.0 ms | 4.0 ms |
| `SYSTEM_TIME` | ~26 | 2.3 ms | 4.5 ms |
| `BATTERY_STATUS` | ~68 | 5.9 ms | 11.8 ms |

`PRIORITY_HIGH` puts the writer level with its own producers, `TaskHeartbeat` and
`TaskMavlinkBatteryStatus`. Both block on `vTaskDelayUntil` every cycle, so despite
time slicing being off they yield regularly and cannot starve the writer.

Alternatives rejected: `PRIORITY_LOW` or lower would let frames accumulate in the
queue, and at 304 heap bytes per `mavlink_message_t` against ~870 free that is a real
risk. Keeping `PRIORITY_HIGHEST` and accepting the stall was rejected because the
stall is invisible from the ground and would be attributed to the radio.

This decision makes `ARCHITECTURE.md`'s priority table and its stated rationale
inaccurate; both are updated in this change.

### 3. The readiness guards are deleted, not replaced

`while (!Serial)` in `src/serial.cpp` (twice) and `waitSerial()` in `src/mavlink.cpp`
(three call sites) exist because the USB CDC is not ready until a host opens the port.
On a `UART`, `operator bool()` returns `true` unconditionally, so after the move they
are not a wait — they are code that always falls straight through.

Nothing replaces them. A hardware UART has no readiness to wait for: the satellite
transmits into the radio whether or not anyone is listening, which is the required
behaviour anyway. Retaining them under the alias would also make the
`-D LINK_SERIAL=Serial` revert build behave differently from the UART build, which is
exactly what the single-definition goal is meant to prevent.

### 4. `TaskSerialRead` keeps its 10 ms poll

The backlog entry *TaskSerialRead polls the port instead of waiting* reasons from "~57
bytes per cycle against a typical 64-byte receive buffer". That figure does not apply
to this core: `SERIAL_BUFFER_SIZE` is 512 and the ring is filled by the receive ISR,
not by the task. At 57600 a 10 ms poll drains ~72 bytes against 512 — roughly seven
times the margin, and the task being late loses nothing because the ISR keeps filling.

Changing the poll is therefore not required by this change and is left alone. The
backlog entry should be re-evaluated against the 512-byte figure rather than
implemented as written.

## Resource ownership

One owner each, unchanged in spirit from `ARCHITECTURE.md`:

| Resource | Owner after this change |
|---|---|
| `LINK_SERIAL` (the UART on D0/D1) | `src/serial.cpp` — the only file that reads or writes it |
| `Serial` (USB CDC console) | **No owner.** Opened in `src/main.cpp`; the only writer is the stack-overflow hook in `src/hooks.cpp`, which runs after the scheduler has stopped. |
| MAVLink protocol semantics | `src/mavlink.cpp` |
| Task and queue creation | `src/main.cpp` |
| SD card, ADC, clocks | unchanged |

The console having no owner is deliberate and is what keeps this change small: a port
that nothing writes to *while tasks are running* needs no arbitration. The overflow
hook is not an exception to that — by the time it runs, nothing else is executing. The
first code that writes to the console from a task must pick an owner in the same
change that introduces it.

## Risks / Trade-offs

- **The UART path is not verifiable on hardware in this change** (no USB-TTL adapter,
  no radio) → `tasks.md` marks it pending hardware. The `-D LINK_SERIAL=Serial` build
  exercises everything except the UART itself, and that limit must be stated rather
  than glossed as verified.
- **The ground station procedure changes and the old one silently stops working** →
  `README.md` is updated in the same change, and the proposal marks it BREAKING.
- **Busy-wait remains, just at a lower priority** → at `PRIORITY_HIGH` it still blocks
  `TaskMavlink`, `TaskLogger` and `TaskSdWrite` for up to ~12 ms per frame. All three
  are periodic or queue-driven with second-scale cadences, so this is acceptable; it
  is recorded here so a future latency-sensitive task does not rediscover it.
- **A future console writer reintroduces the shared-port problem** → mitigated by
  stating in `ARCHITECTURE.md` that `Serial` currently has no owner and that the first
  writer must claim one.
- **`configUSE_TIME_SLICING` is `0` is load-bearing for Decision 2** → if a later
  change enables time slicing, the priority reasoning still holds but for a different
  reason; it is called out here so the link is not lost.

## Open Questions

- The default value of `LINK_BAUD`. 57600 is the convention for SiK-class MAVLink
  telemetry radios and current traffic is ~83 B/s, so bandwidth does not constrain the
  choice. The radio has not been selected, and because the value is a `build_flag` this
  can be settled when it is, without touching the specs, the approach or the tasks.
