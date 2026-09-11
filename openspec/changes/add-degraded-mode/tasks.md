CI runs `pio run` on all three environments, the static-creation grep, and `pio check`.
Nothing else. This change is unusual in how little of it a build can prove: every
requirement is about a reset, a register that survives one, or a task set chosen at boot.
Steps marked **[board]** need the assembled board, and there are many — the board is
unreachable today, so `TODO.md`'s *Recover the bricked board* gates all of them.

## 1. The shared declarations

- [ ] 1.1 Add `include/Recovery.h` declaring the backup-register layout (which `VBTBKR`
  index holds what, with `VBTBKR[0]` marked as the bootloader's and never to be
  written), the boot-phase enumeration, and the reset-reason encoding. Declarations
  only, no state — verify it compiles when included from both `src/main.cpp` and
  `src/mavlink.cpp` and that `pio run` still succeeds
- [ ] 1.2 Add the `PRCR`-unlocked read and write helpers for the backup registers, and
  verify by inspection against `cores/arduino/boot.cpp`, which is the working precedent
  for the unlock sequence

## 2. Read why the board restarted

- [ ] 2.1 Read `RSTSR0`, `RSTSR1` and `RSTSR2` at the top of `setup()`, decode them into
  the reset reason, and clear them so the next boot reads its own cause; verify
  `pio run` succeeds and that the decode covers power-on, watchdog, software and
  external reset with a distinct value each
- [ ] 2.2 **[board]** Verify each cause is reported correctly: power the board (power-on),
  press RESET (external), trigger a software reset, and let the watchdog fire — checking
  after each that the value the firmware reports matches what was done
- [ ] 2.3 **[board]** Verify the registers are cleared: after a watchdog reset, the boot
  that follows the next clean start does not still report a watchdog reset

## 3. The phase marker

- [ ] 3.1 Write the phase marker at each milestone of `setup()` — before anything, after
  the clock, after the link, after the card, after the queues, after the tasks, after the
  scheduler starts — and verify `pio run` succeeds and every milestone has a distinct
  value
- [ ] 3.2 Write a phase marker from both fault hooks in `src/hooks.cpp` before they
  blink, so a watchdog reset that follows a stack overflow or an allocation failure is
  attributable to it; verify `pio run` succeeds and the hooks still contain no `Serial`,
  no `delay()` and no interrupt-dependent wait
- [ ] 3.3 **[board]** Verify the marker names the right step: remove the SD card, let the
  board reset, and confirm the following boot reports the card phase

## 4. The watchdog

- [ ] 4.1 Add `-D WDT_TIMEOUT_MS` to `platformio.ini` with a deliberately short value for
  now, open the WDT in `setup()` with `reset_control` set to reset, and verify `pio run`
  succeeds
- [ ] 4.2 Override `vApplicationIdleHook()` in `src/hooks.cpp` to refresh the watchdog,
  reproducing the port's weak implementation around it — `__disable_irq()`,
  `vTaskSuspendAll()`, `rm_freertos_port_sleep_preserving_lpm(1)`, `__enable_irq()`,
  `xTaskResumeAll()` — with the refresh before the sleep. Verify by comparing against
  `portable/FSP/port.c` that no part of the sleep sequence was dropped: losing it costs
  low-power idle permanently
- [ ] 4.3 **[board]** Verify normal operation never trips it: run the board for longer
  than several `TaskSdWrite` cycles with the card present and confirm no reset occurs
- [ ] 4.4 **[board]** Verify a hang does trip it: stall a task in a non-yielding loop and
  confirm the board resets within the timeout and reports a watchdog cause
- [ ] 4.5 **[board]** Measure how much slower `pio run -t upload` becomes, and confirm it
  still works. The core's DFU path opens the WDT itself and spins when the application
  holds it, so the timeout is the upload latency — this is the step that decides whether
  the flight value is tolerable on the bench
- [ ] 4.6 **[board]** Raise `WDT_TIMEOUT_MS` to the value `TaskSdWrite` actually needs,
  derived from 4.3's observations rather than guessed, and repeat 4.3 and 4.5

## 5. The counters and the boot decision

- [ ] 5.1 Add the consecutive-unstable-boot counter and the cumulative reset counter,
  advancing the consecutive one on watchdog and software resets only, and both on every
  reset; verify `pio run` succeeds and that a power-on or external reset leaves the
  consecutive counter untouched
- [ ] 5.2 Clear the consecutive counter once the firmware has run past the stability
  window, from the existing periodic path rather than a new task, and verify `pio run`
  succeeds and no new task or queue was created
- [ ] 5.3 Make the task set conditional in `setup()`: on three consecutive unstable boots
  or ten cumulative resets, start only the link reader, the link writer, the heartbeat
  and the protocol handler. Verify `pio run` succeeds, that the static storage for every
  task is still declared and counted whether or not it is started, and that
  `scripts/ram_budget.py` reports unchanged headroom
- [ ] 5.4 **[board]** Verify the trigger: force three consecutive resets before the
  stability window and confirm the fourth boot starts reduced
- [ ] 5.5 **[board]** Verify the cumulative path: reset repeatedly, each time after the
  stability window has passed, and confirm the reduced configuration is still reached
- [ ] 5.6 **[board]** Verify a normal boot clears the consecutive counter and the next
  boot is normal

## 6. Absent hardware degrades

- [ ] 6.1 Replace the RTC `configASSERT` with a degradation: record the absence, run on
  ticks since boot, and report epoch time as unset rather than as zero. Verify `pio run`
  succeeds and that `src/mavlink.cpp`'s existing inbound `SYSTEM_TIME` path still sets
  the clock
- [ ] 6.2 Replace the SD `configASSERT` with a degradation: record the absence and do not
  start `TaskLogger` or `TaskSdWrite`. Verify `pio run` succeeds
- [ ] 6.3 Leave the queue and task `configASSERT`s in place as argument checks, and
  document in `ARCHITECTURE.md` that they are no longer part of the boot policy because
  static creation cannot fail for want of memory
- [ ] 6.4 **[board]** Verify with no RTC attached: the board reaches its steady-state
  cadence, reports the absence, and accepts a time set from the ground
- [ ] 6.5 **[board]** Verify with no card attached: the board reaches its steady-state
  cadence, reports the absence, and telemetry continues at normal rates
- [ ] 6.6 **[board]** Verify with neither attached, in the reduced configuration: the
  board still transmits and still receives

## 7. Tell the ground

- [ ] 7.1 Pack the reset reason, the phase reached and both counters into the heartbeat's
  `custom_mode`, which is sent as zero today, and set `system_status` to
  `MAV_STATE_CRITICAL` and drop `MAV_MODE_FLAG_AUTO_ENABLED` from `base_mode` while
  reduced. Verify `pio run` succeeds and that the identity triple is unchanged
- [ ] 7.2 Emit one `STATUSTEXT` at `MAV_SEVERITY_CRITICAL` at boot naming the suspected
  cause, and verify it fits the 50-character field without the pointer-arithmetic defect
  that `TODO.md`'s *Fix the pointer arithmetic in the unknown-message `STATUSTEXT`*
  describes — do not reproduce that pattern here
- [ ] 7.3 **[board]** Verify with MAVProxy that a ground station connecting long after a
  degraded boot sees the reduced state, the cause, the phase and both counters in the
  first heartbeat it receives, having issued no request
- [ ] 7.4 **[board]** Verify normal operation reports as operational, and that the message
  set and rates match the nine HIL cases

## 8. Leave the reduced configuration

- [ ] 8.1 Act on `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` in the existing `COMMAND_LONG` case:
  clear both counters and reset. Verify `pio run` succeeds and that the command is
  acknowledged rather than silently handled
- [ ] 8.2 Add the long automatic retry so a board whose link is the failing part is not
  stranded, with an interval long enough that it cannot oscillate. Verify `pio run`
  succeeds and the interval is a named constant, not a literal
- [ ] 8.3 **[board]** Verify the ground command returns the board to normal
- [ ] 8.4 **[board]** Verify the automatic retry fires after the interval, and that a
  board that fails again returns to the reduced configuration rather than cycling rapidly

## 9. Documents, backlog and verification

- [ ] 9.1 Update `ARCHITECTURE.md`: the boot decision, the reduced task set, the phase
  marker, the watchdog and where it is refreshed, and the backup-register layout. Add the
  watchdog and the backup registers to the resource table in section 6 with their owners
- [ ] 9.2 Rewrite the `configASSERT` convention in `CLAUDE.md` rather than amending it,
  and add that `VBTBKR[0]` belongs to the bootloader and must never be written by this
  firmware
- [ ] 9.3 Update the `context` block in `openspec/config.yaml`, which states that
  `configASSERT` halts on a missing RTC or SD card — it is injected into every future
  proposal
- [ ] 9.4 Delete the three `TODO.md` entries this change consumes — *Add a watchdog*,
  *`setup()` asserts on the RTC before the console exists*, and *Report the satellite's
  real state in the heartbeat* — and re-point anything that cross-references them at this
  change id, verifying no reference is left dangling
- [ ] 9.5 Verify `openspec validate add-degraded-mode --strict`, `pio run` on all three
  environments, the static-creation grep, and `pio check` with no new defects against the
  12 pre-existing LOW ones
- [ ] 9.6 **[board]** Run `pio test` and verify all nine HIL cases pass in the normal
  configuration. Without a board every case reports `IGNORE` and the command exits 0, so
  a green run off the board proves nothing
- [ ] 9.7 **[board]** Verify `VBTBKR` survives what this change needs it to: reset the
  board and confirm the counters persist. Then remove power entirely and confirm whether
  they do — the design does not depend on that, and the auto-retry interval must not be
  trusted until it is known either way
- [ ] 9.8 **[board]** Clear both counters after commissioning, since they survive
  reflashing and a firmware flashed over a board that had counted failures starts from
  that count
