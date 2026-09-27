# Proposal

## Why

When a task overflows its stack or an allocation cannot be satisfied, the ground learns
*that* it happened — the phase marker rides the next boot's heartbeat — but never *which
task* it was, and that is the one fact needed to act on it from the ground. The hooks
themselves are also wrong in ways the `TODO.md` entry *The stack overflow hook hangs before
it warns* only half-described: since `add-degraded-mode` opened a watchdog the board no
longer hangs, it resets within the watchdog period, but the overflow hook still spins on
`while (!Serial)`, writes ASCII into a binary MAVLink stream, builds a `String` and calls
`delay()`, all with interrupts masked; and the LED patterns `ARCHITECTURE.md` documents for
both hooks never appear, because neither fits inside the watchdog period.

## What Changes

- Both fault hooks record the phase marker **and the name of the task that faulted** in the
  battery-backed recovery block, then reset the board immediately. They no longer drive the
  LED, touch the serial port, allocate, or wait. The allocation-failure hook records a fixed
  name when it is reached before the scheduler has started.
- **BREAKING (to the documented bench behaviour):** the two LED patterns are removed. A
  fault is no longer signalled on the board's indicator; it is reported to the ground on the
  next boot. `LED_BUILTIN` is also the SD card's SPI clock, so no indication can persist
  while the board runs.
- The boot `STATUSTEXT` names the faulting task after a fault-hook reset, using the full
  50-character field.
- **MAVLink surface change:** `STATUSTEXT` (253) joins the set `MAV_CMD_REQUEST_MESSAGE`
  serves. A request is answered with the boot reset statement, so a ground station that
  connected late — or over USB, which cannot observe boot-time text across a reset — can
  still obtain it. `MAV_CMD_GET_MESSAGE_INTERVAL` reports it as a message with no stream.
  No new message id, stream rate or identity change.
- Every log file opening writes a new DataFlash `RST` record after the head `TIME`:
  the reset cause, the phase the previous boot reached, both fault counts and the faulting
  task. The head `TIME` stays at its fixed offset, so `LOG_ENTRY.time_utc` keeps reading
  files already on the card.
- `src/hooks.cpp` stops writing to the USB port, which removes the one exception to "only
  `src/link.cpp` touches the MAVLink ports".
- Two build-time fault-injection switches, passed by hand through
  `PLATFORMIO_BUILD_FLAGS` and absent from every environment, so the hooks can be exercised
  on the board.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `memory-budget`: *An allocation that cannot be satisfied stops the firmware* no longer
  requires an indication on the board's indicator that persists until reset; it requires
  the firmware to stop emitting, reset itself, and report the failure and the task at the
  next boot.
- `fault-recovery`: *The cause of a reset is known at the next boot* additionally names the
  task a fault handler was reached in; *The ground can see the state without asking for it*
  makes the human-readable statement name that task and be obtainable on request.
- `mavlink-link`: *`MAV_CMD_REQUEST_MESSAGE` sends the requested message once* adds
  `STATUSTEXT` to the served set; *`MAV_CMD_GET_MESSAGE_INTERVAL` reports the interval a
  message really has* reports it as request-only.
- `flight-log`: adds *The reset that started this boot is recorded in the log*.

## Impact

- **Code:** `src/hooks.cpp` (both hooks), `include/Recovery.h` and `src/recovery.cpp` (a
  task-name field inside the checksummed block), `src/main.cpp` (captures the previous
  boot's task name beside `previousBootPhase`), `src/mavlink.cpp` (boot statement,
  `kMessages` row for 253), `src/sdwrite.cpp` (`RST` format and record in the preamble
  callback), `src/logger.cpp` (the injection point, compiled only under the switch).
- **RAM:** no task, queue or library is added. `.bss` grows by the copied task name.
  `TaskSdWrite`'s preamble callback gains an `RST` record on its stack and `TaskMavlink`
  builds a longer text; both high-water marks must be read after the change, since both
  stacks are tuned tight. The backup-register block grows by the name, which is not RAM.
- **Recovery block compatibility:** widening the checksum means the first boot of this
  firmware may find the block invalid and reinitialise it — both counts cleared, the reset
  reported once as "backup state invalid". Commissioning clears them anyway.
- **Docs:** `ARCHITECTURE.md` (the LED table and the text around it, the port-ownership
  exception, section 3's "not finished" paragraph), `CLAUDE.md` (the reference to that
  exception), `TODO.md` (the entry is deleted; add-degraded-mode's 3.2/3.3 leftover,
  *Minor leftovers cleanup*'s missing-space item and *Debug and release builds*' mention
  of the slow blink are re-pointed or removed; *The boot `STATUSTEXT` cannot be observed
  over USB after a reset* is narrowed to the clock report).
- **Verification limits:** the allocation-failure path is exercised by calling the hook,
  not by a failing allocation — the heap is zero and CI forbids naming the allocator in
  `src/`. The `RST` record exists only when the log runs, so a boot in the reduced
  configuration or without a card has no `RST`; the requestable `STATUSTEXT` covers that
  case. Every hook scenario needs the assembled board with an injection build flashed.
