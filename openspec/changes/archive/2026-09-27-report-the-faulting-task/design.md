# Design

## Context

See `proposal.md` for why. What shapes the approach:

- **Both hooks run with the scheduler's guarantees gone.** The stack-overflow check
  (`configCHECK_FOR_STACK_OVERFLOW=2`) calls its hook from the context switch, in the
  PendSV handler, on the main stack, not on the task stack that overflowed. The
  allocation-failure hook runs in the calling task's context. In both, memory next to the
  faulting stack may already be corrupt. Whatever the hooks do has to work with interrupts
  masked, with no tick, and with as little code as possible.
- **The watchdog already bounds both hooks.** `src/main.cpp` opens the WDT at
  `WDT_TIMEOUT_MS` (1398 ms today) with `stop_control` disabled, and only the idle hook
  refreshes it. A hook that masks interrupts and spins is reset within one period. That is
  why the current hooks "work", why neither LED pattern is ever seen whole, and why the
  overflow hook's `delay(2000)` never returns.
- **`LED_BUILTIN` is D13, which is also `PIN_SPI_SCK`** (the MINIMA variant's
  `pins_arduino.h`). The indicator belongs to the card's bus in normal operation.
- **The recovery block** is `VBTBKR[4..11]`: magic, a byte-sum checksum over `[6..11]`,
  the two counts, the snapshot, the phase, the reason and the deliberate-reset marker.
  `src/recovery.cpp` is the only code that touches the registers; `src/main.cpp` is the
  only writer of content, and `src/mavlink.cpp` reads it. The hooks already call
  `Recovery::setPhase()`, which makes them a second writer. They were one before this
  change too.
- **`main.cpp` captures `previousBootPhase` before this boot overwrites it.** Everything
  that reports the previous boot reads that global, not the register.
- **The log head is fixed at 356 bytes.** `LOG_ENTRY.time_utc` is read from the `TIME`
  record at `sizeof(kPreamble)`, pinned by a `static_assert`. `writeLogPreamble()` runs
  when a file is opened: at boot, when it is reopened after a power cycle, and on every
  rotation.
- **`kMessages`** is the one table behind `MAV_CMD_REQUEST_MESSAGE` and
  `MAV_CMD_GET_MESSAGE_INTERVAL`. A row with a `send` function and `kNoScheduleRow` is
  served on request and reported as `-1` by the interval command, with no further code.
- **`LinkMsg.statustext.text` is `char[50]`.** `sendStatusText()` forces a terminator into
  the last byte, which caps the text at 49 characters. MAVLink's field is 50 characters,
  and a text that fills it carries no terminator.

## Goals / Non-Goals

**Goals:**

- Hooks that record and reset. Nothing they do depends on interrupts, the tick, the
  serial port, allocation or the LED.
- The faulting task's name reaches the ground by three routes: the boot statement, that
  statement on request, and the log.
- The fault paths can be exercised on the board without a new environment.

**Non-Goals:**

- Surviving or diagnosing the corruption an overflow caused. The board resets, and the
  report says where the fault happened, not what it damaged.
- Putting the task name in the heartbeat. `custom_mode` has no room for it.
- Making the other boot-time text, the clock report, requestable. That stays with
  *The boot `STATUSTEXT` cannot be observed over USB after a reset* in `TODO.md`.
- Keeping any LED indication, at fault time or at the next boot. This was considered and
  declined; see D1.
- Making the log available in the reduced configuration.

## Decisions

### D1. Record, then `NVIC_SystemReset()`; no indicator

Each hook does three things. It masks interrupts. It records the fault kind (the existing
phase values `StackOverflowFault` / `MallocFailedFault`) and the task name in one
recovery-block write. Then it calls `NVIC_SystemReset()`. The reset is recorded as
`Software` with no deliberate-reset marker, so it advances the consecutive count exactly
as the watchdog reset did before. The phase byte is what tells it apart from other
software resets.

Alternatives considered:

- **Leave the reset to the watchdog.** Correct but slower, and the reset cause then
  depends on the watchdog period instead of on the hook. Nothing is gained by waiting.
- **Blink in the hook, refreshing the WDT by hand, then reset.** This adds a second
  place that refreshes the watchdog, and more code runs at the moment memory is least
  trustworthy.
- **Blink at the next boot, before `SD.begin()`.** Workable, but it delays every boot
  after a fault, and on a board that flies nobody sees it. Declined in exploration.
- **Blink while running.** Impossible without taking SCK away from `src/sdwrite.cpp`.

### D2. The task is recorded by name, in `VBTBKR[12..26]`

Fifteen bytes, which is `configMAX_TASK_NAME_LEN - 1`. The name is zero-padded and
truncated past fifteen characters. The checksum and `reinitialise()` widen to cover
`[6..26]`, so a corrupted name is detected like any other corrupted field.

`Recovery` gains one call that writes the name and the phase under a single PRCR unlock
and recomputes the checksum once. The name is written first and the phase last, so a
reset that lands in between leaves a phase that does not claim a fault.

Nothing clears the name. It is meaningful only while the phase byte holds a fault value,
and every boot overwrites the phase from `Start` onwards. A later, unrelated reset
therefore reports its own phase, and the stale name is never shown.

Alternative: a numeric id, found by comparing the handle against every task handle. It
takes one byte, but it adds a fifth edit to "adding a subsystem is four edits", one that
nothing checks. A task that is not in the table (IDLE, or one added later) would come out
as "unknown". The name needs no table.

### D3. Where each hook gets the name

- **Stack overflow:** from the `pcTaskName` argument. It points into the TCB, which is
  static and separate from the stack array. An overflow that ran far enough to corrupt a
  neighbouring TCB can still spoil it, so the copy stops at a NUL or after fifteen bytes,
  whichever comes first.
- **Allocation failure:** from `pcTaskGetName(NULL)` when `xTaskGetSchedulerState()`
  reports the scheduler started. Otherwise it records the fixed name `setup`. Checking the
  scheduler state, and not merely whether a task exists, matters: once the first
  `xTaskCreateStatic` has run, `pxCurrentTCB` points at a created task even though none is
  running, so `pcTaskGetName(NULL)` would name the wrong task.

Every reader replaces a byte outside printable ASCII with `?` and stops at the first NUL.
That covers what the specs mean by "as far as it is legible".

### D4. `main.cpp` snapshots the previous boot's fault, next to `previousBootPhase`

It adds a `char previousFaultTask[16]` and the two counts as this boot set them, all
captured in `setup()` where `previousBootPhase` is. Readers use these globals, never the
registers. Snapshotting the counts matters for the log: the consecutive count is cleared
once the boot is declared stable, and a file rotated after that must still describe the
boot as it began. The cost is 18 bytes of `.bss`.

### D5. Boot statement: one text, built on demand, 50 characters

`sendBootStatusText()` splits into a builder and a sender that takes a port. At boot it is
called for both ports, as today. The new `kMessages` row `{STATUSTEXT, sendBootStatement,
kNoScheduleRow}` calls it for the requesting port. The text is rebuilt from the D4
globals each time and never stored. The same globals always give the same text, so there
is nothing to keep in sync.

Format:

- Any non-fault phase, unchanged: `Reset: <reason>, phase <phase>`.
- A fault phase: `Reset: <reason>, overflow <task>` or `Reset: <reason>, malloc <task>`.

Worst case: `Reset: ` (7) + `external/unknown` (16) + `, ` (2) + `overflow ` (9) + 15 =
49. The one longer reason, `backup state invalid`, cannot occur together with a fault
phase, because it means the block was just reinitialised and the phase reads `Start`.
The builder still bounds every concatenation, so an impossible combination truncates
instead of overrunning.

`sendStatusText()` stops forcing a terminator. It copies up to 50 bytes, NUL-padding a
shorter text, as the MAVLink field defines. The other callers pass shorter texts and see
no difference.

Alternative: a second `STATUSTEXT` carrying only the task. Declined in exploration in
favour of one statement.

### D6. `RST` goes after `TIME`, in every file

`writeLogPreamble()` appends two records after the head `TIME`: an `FMT` that defines
`RST`, then one `RST`. DataFlash requires a definition only before its first use, not in
the head, so `kPreamble`, the 356-byte offset and its `static_assert` stay unchanged.
Files already on the card keep a correct `time_utc`.

Record `RST`, format `QBBBBN`, columns `TimeUS,Rsn,Phase,Cons,Cum,Task`:

- `Rsn` and `Phase` are the `Recovery` enum values, the same numbers the heartbeat
  packs.
- `Cons` and `Cum` are the D4 snapshots.
- `Task` is the filtered name when the phase is a fault, and empty otherwise.

It is written on every opening, rotations included. Two `RST` records with the same `Cum`
describe the same boot, which is what `flight-log`'s new requirement relies on.

The record is built on `TaskSdWrite`'s stack inside the preamble callback, which adds
about 31 bytes to a stack tuned at 320 words. Its high-water mark has to be read after
the change (see tasks).

Alternative: write `RST` only in the first file of each boot. Declined: if the ring
later overwrote that file, the boot's reason would be lost, and every file is meant to
be readable on its own.

### D7. Fault injection: `-D INJECT_FAULT=<n>`, only by hand

`INJECT_FAULT` is never set in `platformio.ini`. It is passed as
`PLATFORMIO_BUILD_FLAGS='-D INJECT_FAULT=<n>' pio run -t upload`.

| n | Where | What |
|---|---|---|
| 1 | `TaskLogger`, about 10 s after it starts | writes over the bottom words of its own stack array, the guard bytes the method-2 check inspects, then yields so the check runs |
| 2 | `TaskLogger`, about 10 s after it starts | calls the allocation-failure hook directly |
| 3 | `setup()`, after queue creation, before the first task | calls the allocation-failure hook directly |

Every variant fires only when the consecutive count this boot started with is 0. The boot
after the fault therefore runs clean and reports it. The HIL case sets that up with
`MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, which clears both counts. Without this gate,
variant 3 would reset on every boot before the link ever came up.

Choices:

- **`TaskLogger`:** it is low priority and exists only in the normal configuration with a
  card, so no stray injection can outlive the reduced configuration.
- **The guard bytes and not a real deep recursion:** the write stays inside the task's
  own stack array, so nothing next to it is corrupted. The trade-off is that it tests
  the hook, not the detection of a genuine overflow.
- **Calling the hook for malloc:** the heap is zero and CI rejects the allocator's name
  anywhere in `src/`. The path from a failing allocation to the hook is FreeRTOS's, and
  this change does not exercise it.

A CI step greps `platformio.ini` for `INJECT_FAULT` and fails if it is there, so an
injection build cannot become the flight build by accident.

Alternative: a MAVLink command that triggers the fault. Rejected: it puts
self-destruct code in the flight image.

### D8. Ownership

| Resource | Owner | This change |
|---|---|---|
| `VBTBKR[4..26]` | `src/recovery.cpp` (access), `src/main.cpp` (content) | both hooks keep writing through `Recovery`, as before; the name adds a field, not a writer |
| Both MAVLink ports | `src/link.cpp` | `src/hooks.cpp` stops writing to USB, so the exception goes away |
| The card | `src/sdwrite.cpp` | writes `RST`; reads only the D4 globals |
| `LED_BUILTIN` / SCK | `src/sdwrite.cpp` via SPI | the hooks stop driving it |

## Risks / Trade-offs

- **[Widening the checksum invalidates the block once]** → The first boot of this
  firmware reads `[12..26]` as whatever the registers hold. Unless that sums to zero, it
  reinitialises: both counts are cleared and the reset is reported as `backup state
  invalid`. Flashing the older firmware back causes the same thing in reverse. This is
  stated in the proposal. Commissioning clears the counts anyway.
- **[A task name spoiled by the overflow]** → D3's bounded copy and the reader's filter.
  In the worst case the report names garbage as `?` characters, but it still says which
  fault happened.
- **[`TaskSdWrite` or `TaskMavlink` stack growth]** → Both high-water marks are checked on
  the board after the change: `SdWrite` from the housekeeping stream, `Mavlink` the same
  way. `RST` adds about 31 bytes on `SdWrite`, and the text builder moves from a 50-byte
  buffer to one of 50 or 51 bytes.
- **[Injection code in the flight source]** → Compiled only under `INJECT_FAULT`. The CI
  grep of D7 keeps it out of `platformio.ini`, and `scripts/ram_budget.py` does not care
  either way.
- **[Variant 1 does not test detection of a real overflow]** → Accepted and stated. The
  check itself is FreeRTOS's and was not written here.
- **[The name reaches the ground only if someone asks or listens]** → The heartbeat still
  carries the fault phase continuously, so the fault itself is never missed. Only the
  task name needs a request, a UART listener at boot, or the log.
- **[`STATUSTEXT` on request is unusual]** → No GCS will ask for it by itself. MAVProxy:
  `long MAV_CMD_REQUEST_MESSAGE 253`. The HIL case is the documented client.

## Migration Plan

Flash, then send `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`. That clears the one-time
`backup state invalid` boot and restarts the board with clean counts. Rolling back is a
reflash of the previous firmware, with the same one-time effect.
