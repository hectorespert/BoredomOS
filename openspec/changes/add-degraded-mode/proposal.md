## Why

Nothing in this firmware turns a fault into a recovery. A task that hangs — an
`xQueueReceive` that never arrives, an I2C transfer waiting for the DS1307, either of
the two fault hooks in `src/hooks.cpp` — leaves the satellite inert until a power
cycle that nobody in orbit can perform. A `HardFault` goes to the FSP default handler
and stops. `configASSERT` on a missing RTC or SD card halts in `setup()` before the
link is open, so the ground never learns why.

This was rehearsed on the bench. A firmware 72 bytes short of the heap bricked the
board in a way that needed physical access to the RESET button, and the board is still
unreachable. In orbit there is no RESET button, and the same class of failure is
permanent.

A watchdog alone does not fix it. It reboots into the same firmware, so a fault
present at boot becomes an endless reset loop — which `TODO.md`'s *Add a watchdog*
already identifies as the open question, and the incident answers: worse, not better.
What is missing is that a reset should *mean* something. Three facts make that
possible, and none of them is used today:

- **`R_SYSTEM->RSTSR0/1/2`** already record why the chip reset — power-on, watchdog,
  software, or the RESET pin. Nothing in the BSP or the core clears them, so they
  arrive intact in `setup()`. The reason does not have to be inferred.
- **`R_SYSTEM->VBTBKR`** is 512 bytes of battery-backed register that survive any
  reset. The bootloader uses exactly one of them, `VBTBKR[0]`, for the double-tap
  magic. The other 511 are untouched by this firmware and by the bootloader.
- **`configUSE_IDLE_HOOK` is already 1**, and the idle task runs only when every
  other task is blocked. Refreshing a watchdog from there detects a task that stops
  yielding, with no shared state and no per-task bookkeeping.

The `use-static-allocation` change removed one of the three failure classes this would
otherwise have to cover: a firmware whose kernel objects do not fit now fails to link
rather than dying at boot. What remains is hardware that is absent or dead, and a task
that crashes or hangs — and for both of those the answer is to reboot, remember, and
start something smaller.

## What Changes

- **An independent watchdog.** The RA4M1 WDT is opened in `setup()` and refreshed from
  `vApplicationIdleHook()`, so a task that stops yielding stops the refresh. The
  timeout is set against the slowest legitimate cycle — `TaskSdWrite`, which blocks on
  SPI. This consumes `TODO.md`'s *Add a watchdog*.
- **The reset reason is read, not guessed.** `RSTSR0/1/2` are read and cleared at the
  top of `setup()`, distinguishing a power-on from a watchdog reset, a software reset
  and a RESET-pin press.
- **Two counters and a phase marker in the backup registers.** A count of consecutive
  boots that never reached stability, a cumulative count that only the ground clears,
  and a one-byte marker of how far the last boot got. The marker is what makes a halt
  in `setup()` diagnosable: reset at the SD phase names `SD.begin(9)` as the suspect.
- **A degraded boot.** After three consecutive unstable boots, or ten cumulative
  resets, the firmware starts only what keeps it reachable: the link reader, the link
  writer, the heartbeat and the protocol handler. Housekeeping, the SD card and battery
  telemetry do not start. The protocol handler is not optional — without it there is no
  way to command an exit, and a state you can only leave through a link that may be
  broken is a trap rather than a safe mode.
- **`configASSERT` on the RTC and the SD card becomes a degradation.** **BREAKING** for
  the boot policy that `CLAUDE.md` defends, and this is the explicit decision it asks
  for. Asked the other way round — what does the firmware need in order to be
  reachable? — neither is required. Without a DS1307 the board runs on relative time
  and the ground sets the clock with `SYSTEM_TIME`, which `src/mavlink.cpp` already
  handles. Without a card the housekeeping log stops and nothing else notices. Both
  failures are reported rather than fatal. This consumes `TODO.md`'s *`setup()` asserts
  on the RTC before the console exists*.
- **The state reaches the ground in every heartbeat.** `system_status` becomes
  `MAV_STATE_CRITICAL` when degraded, `base_mode` drops `MAV_MODE_FLAG_AUTO_ENABLED`,
  and `custom_mode` — a `uint32_t` sent as zero today — carries the reset reason, the
  phase reached, and both counters. A `STATUSTEXT` at `MAV_SEVERITY_CRITICAL` names the
  suspect once at boot. This consumes `TODO.md`'s *Report the satellite's real state in
  the heartbeat*, which until now had no real state to report.
- **Two ways out.** `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, which already reaches the
  `COMMAND_LONG` case in `src/mavlink.cpp`, clears the counters and resets. And a long
  automatic retry, so a satellite whose link is the broken part is not stranded.

Not changed: the MAVLink identity triple, the message set, the stream rates in normal
operation, and the queue or memory model. `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` becomes a
command the firmware acts on rather than ignores, which is a new inbound behaviour but
not a new message id — `COMMAND_LONG` is already received and decoded.

## Capabilities

### New Capabilities
- `fault-recovery`: how a hang becomes a reset, how a reset is explained, how repeated
  failures select a reduced configuration, what that configuration must still do, and
  how all of it reaches the ground.

### Modified Capabilities

None. `mavlink-link` fixes the port, speed and rates, and none of those move;
`memory-budget` governs where memory is reserved, and a task that is not started still
has its storage counted by the linker.

## Impact

- **`src/main.cpp`** — the reset reason, the counters and the phase marker at the top of
  `setup()`; the task set becomes conditional; the RTC and SD asserts become
  degradations. This is the file the change turns from a straight line into a decision,
  and the risk is concentrated here.
- **`src/hooks.cpp`** — `vApplicationIdleHook()` to refresh the watchdog. The port's
  weak implementation does `__disable_irq()` and `rm_freertos_port_sleep_preserving_lpm(1)`;
  overriding it without reproducing that loses low-power idle, which matters on a
  satellite. Both fault hooks also gain a phase-marker write before they blink, so a
  watchdog reset that follows one is attributable.
- **`src/mavlink.cpp`** — the heartbeat fields, the `STATUSTEXT` at boot, and the
  reboot command.
- **`include/`** — a new header for the backup-register layout and the phase
  enumeration, so `src/main.cpp` and `src/mavlink.cpp` agree on it without either
  owning it.
- **`platformio.ini`** — the WDT timeout as a build flag, so the bench and flight
  timeouts can differ.
- **RAM**: no new task and no new queue. The backup registers are peripheral
  registers, not RAM. The counters and the reset reason are a handful of bytes of
  `.bss`; against the 2652 bytes of headroom the linker now polices, and with
  `scripts/ram_budget.py` failing the build if that runs out, this is not the
  constraint it would have been before.
- **`pio run -t upload` gets slower, and this must be verified.** The core's
  1200-baud DFU path opens the WDT itself, and when the application already holds it
  the path spins deliberately, waiting for the refresh to stop. The application's
  timeout therefore becomes the upload latency. A long timeout chosen for `TaskSdWrite`
  is a long wait on every flash.
- **`ARCHITECTURE.md`** gains the boot decision and the degraded task set; `CLAUDE.md`'s
  `configASSERT` convention is rewritten rather than amended.
- **Not verifiable without the board, and more dependent on it than any change so far.**
  A build proves nothing here: every behaviour is a reset, a register that survives one,
  or a task set chosen at boot. The board is currently unreachable — see `TODO.md`'s
  *Recover the bricked board* — and this change should not be flashed until it is, then
  flashed first with a known-good escape route.
