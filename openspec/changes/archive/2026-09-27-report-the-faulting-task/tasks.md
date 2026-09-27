# Tasks

Marks: **[board]** needs the assembled board attached; **[hands]** needs someone at the
board as well, doing something no script can do. Unmarked steps are checked by CI alone
(`pio run`, the static-creation and allocator greps, `pio check`).

## 1. The recovery block carries the task name

- [x] 1.1 In `include/Recovery.h`, add `OFFSET_FAULT_TASK = 12` and `FAULT_TASK_LEN = 15`, move the end of the checksummed range to 26, and add one call that records a fault phase and a task name together (design D2). Fix the stale `LINK_SERIAL.begin()` comment at the top of the phase enum while you are there. Verify: `pio run` builds.
- [x] 1.2 In `src/recovery.cpp`, widen `computeChecksum()` and `reinitialise()` to `[6..26]`, and implement the new call as one PRCR unlock: name bytes first (zero-padded, truncated at 15, stopping at a NUL), phase last, checksum once. Verify: `pio run` builds, and `pio check` reports no new finding against the current baseline.
- [x] 1.3 **[board]** Flash, and confirm the one-time migration the design predicts. The first heartbeat reports `backup state invalid` (or a clean reason if the registers happened to sum to zero; record which). After `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, the next boot reports `software` with both counts 0. Verify: `check_recovery.py` passes on the boot after the reboot command.
  - *Result (2026-09-27):* the first boot reported reason 5, `backup state invalid`, as predicted. After the reboot command it reported `software`, consecutive 0 and cumulative 1. The cumulative count is 1, not 0, because the command's own reset counts, as it always has. `check_recovery.py`'s operational and message-set cases pass.

## 2. Hooks that record and reset

- [x] 2.1 Rewrite `vApplicationStackOverflowHook` in `src/hooks.cpp`: mask interrupts, record `StackOverflowFault` with `pcTaskName` through 1.1's call, then `NVIC_SystemReset()`. Remove `Serial`, `String`, `pinMode`/`digitalWrite` and `delay` from the file. Verify: `grep -nE 'Serial|String|digitalWrite|delay\(' src/hooks.cpp` finds nothing, and `pio run` builds.
- [x] 2.2 Rewrite `vApplicationMallocFailedHook` the same way. The name is `pcTaskGetName(NULL)` when `xTaskGetSchedulerState()` says the scheduler has started, and `setup` otherwise (design D3). Delete `spinMilliseconds()`, which nothing uses any more. Verify: `pio run` builds, and the allocator grep in CI still passes.
- [x] 2.3 Update `ARCHITECTURE.md`: remove the status-LED table and the paragraph after it (section 6); rewrite the paragraph around `configCHECK_FOR_STACK_OVERFLOW=2` so that a fault hook records and resets instead of blinking; delete the "one exception" from the ports row of the ownership table and the sentence saying the overflow hook still writes to USB; replace section 3's "not finished" paragraph about the hooks relying on the watchdog; record `VBTBKR[12..26]` in the backup-register row. Verify: `grep -nE 'blink|one exception|writes text to the USB' ARCHITECTURE.md` finds nothing left over about the hooks.
- [x] 2.4 Update `CLAUDE.md`: `src/link.cpp` owns both ports with no exception, so drop "see `ARCHITECTURE.md` section 6 for the one exception". Verify by reading the paragraph back.

## 3. The previous boot's fault is captured, and can be injected

- [x] 3.1 In `src/main.cpp`, beside `previousBootPhase`, capture `previousFaultTask[16]` (filtered: any non-printable byte becomes `?`, stopping at the first NUL) and snapshot the consecutive and cumulative counts once `updateCountersAndDecideConfiguration()` has set them (design D4). Verify: `pio run` builds, and `scripts/ram_budget.py` shows `.bss` grew by the expected ~18 B and headroom stays above the floor.
- [x] 3.2 Add the `INJECT_FAULT` variants from design D7. Variant 3 goes in `setup()` after queue creation. Variants 1 and 2 go in `src/logger.cpp`, about 10 s after the task starts. All three fire only when this boot's snapshotted consecutive count is 0. Variant 1 writes over the bottom words of `loggerStack` and yields. Verify: `pio run` with no flag builds an image the same size as without the change's injection code, and `PLATFORMIO_BUILD_FLAGS='-D INJECT_FAULT=1' pio run` builds.
- [x] 3.3 Add a CI step to `.github/workflows/main.yml` that fails if `platformio.ini` mentions `INJECT_FAULT`. Verify: the step passes on the branch; a local copy of `platformio.ini` with the flag added makes the same grep fail.

## 4. The boot statement names the task and is served on request

- [x] 4.1 In `src/mavlink.cpp`, make `sendStatusText()` copy up to 50 bytes without forcing a terminator. Split `sendBootStatusText()` into a builder that uses the D4 globals and the D5 format, and a sender that takes a port. Boot still calls it for both ports. Verify: `pio run` builds.
- [x] 4.2 Add `{ MAVLINK_MSG_ID_STATUSTEXT, sendBootStatement, kNoScheduleRow }` to `kMessages`. Verify: `pio run` builds.
- [x] 4.3 In `test/test_hil/check_message_requests.py`, add 253 to the served-id loop and `(253, -1)` to the interval-of-on-request case. Add a case that requests 253 and checks that the text starts `Reset: ` and names the same reason `custom_mode` carries. **[board]** Verify: `pio test` passes these cases over USB.
- [x] 4.4 Add `test/test_hil/check_faults.py`, gated on `HIL_FAULT_INJECTED=<n>`, which states the variant flashed, and self-skipping without it. The case sends `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, waits for the fault reset, reopens the port, then checks the heartbeat phase (7 or 8), and checks `REQUEST_MESSAGE 253` returns `overflow Logger`, `malloc Logger` or `malloc setup` according to the variant. Document the procedure in `test/test_hil/README.md`, including the restore step: reflash with `pio run -t upload`, then `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`. Verify: `run.py --list` shows the case, and it self-skips under plain `pio test`.
- [x] 4.5 **[board]** Run 4.4 for each of the three variants: flash with `PLATFORMIO_BUILD_FLAGS='-D INJECT_FAULT=<n>' pio run -t upload`, then run `HIL_FAULT_INJECTED=<n> test/test_hil/run.py --filter <case>`, then restore. Verify: all three pass, and the restore leaves `check_recovery.py`'s operational case passing.
  - *Result:* variants 1, 2 and 3 each PASS over USB. After restoring (reflash with no flags, then the reboot command) the board reports reason `software`, consecutive 0, normal configuration, and the full suite in 6.2 passes.
- [ ] 4.6 **[board]** Variant 1 over the UART, with an adapter on D0/D1: the unsolicited boot `STATUSTEXT` itself reads `Reset: software, overflow Logger`. This is the `fault-recovery` scenario *The boot statement names the faulting task*, and it cannot be observed over USB. If no adapter is available, leave this unticked and say so.
  - *Not run:* no USB-TTL adapter was attached this session (only the board's own CDC port was present). The same text was observed over USB on request in 4.5, but the unsolicited boot emission over the UART is unobserved.
- [x] 4.7 Update `CLAUDE.md`'s HIL paragraph: the case and module counts, and one more self-skipping case with what it needs. Verify: the counts match `run.py --list`.

## 5. The log records every boot

- [x] 5.1 In `src/sdwrite.cpp`, add the `RST` `FMT` and record from design D6, written in `writeLogPreamble()` after `TIME`. Leave `kPreamble` and its `static_assert` untouched. Verify: `pio run` builds with the `static_assert` still at 356.
- [x] 5.2 **[board]** Download the file being written over the log protocol, parse it with pymavlink's DataFlash reader, and confirm that `RST` follows `TIME` with `Rsn` and `Cum` matching the heartbeat. Add this as a case in `check_log_download.py` if that suite can reach the file cheaply; otherwise do it by hand and record the result here. Verify: the case, or the recorded result.
- [x] 5.3 **[board]** After 4.5's variant 1 run, download or pull the log and confirm that the boot after the fault wrote `RST` with the overflow phase and `Task` = `Logger`. Verify: recorded here.
  - *Result:* log 2's last `RST` after the variant 1 run was `Rsn=3 Phase=7 Cons=1 Cum=2 Task='Logger'`. The earlier `RST` records in the same file show each boot in between: the reboot command's boot, the injection flash's boot, and the HIL case's reboot.
- [x] 5.4 **[board]** Confirm that `LOG_REQUEST_LIST` still reports the same `time_utc` for a file written before this change. Verify: list before flashing, list after, compare.
- [ ] 5.5 **[board] [hands]** Force a rotation (or wait for one) and confirm that the new file carries an `RST` with the same `Cum` as the previous one. If a rotation cannot be reached in a session, leave this unticked and say so.
  - *Not run:* the file being written was about 605 KB against a 1 MiB rotation, at about 34 B/s, so a natural rotation was hours away, and nothing forces one. Rotation calls the same `writeLogPreamble()` callback as boot, so the code path is the one 5.2 exercised, but writing `RST` after a rotation has not been observed.

## 6. Integration

- [x] 6.1 **[board]** Read the `SdWrite` and `Mavlink` stack high-water marks from the housekeeping stream after the change, and compare them with a reading from the previous firmware. Verify: both still have headroom, with the figures recorded here.
  - *Result (words free, minimum-ever):* before the change, on a board that had been up for a while, `Mavlink` 105 and `SdWrite` 89. After the change and a full `pio test` (log downloads included), `Mavlink` 104 and `SdWrite` 104. The older board's `SdWrite` figure came from a longer uptime, which may have included a rotation, so 104 against 89 is not an improvement; it only shows the `RST` record did not push the boot path close. Rotation has still not been measured (see 5.5).
- [x] 6.2 **[board]** Full `pio test` on the flight firmware, restored to the normal configuration. Verify: no failures; the self-skips are the documented ones.
  - *Result:* `test_hil` PASSED in 278 s: 57 cases passed and none failed. The remaining 12 of the 69 self-skipped: the adapter, reduced-configuration, manual-reset, clock-reset and fault-injection cases.
- [x] 6.3 `pio check`: no finding beyond the baseline. Verify: the count is recorded here.
  - *Result:* 2 LOW, the same as the baseline on `master`. One new finding, `constParameterPointer` on the overflow hook's `pcTaskName`, is suppressed inline: FreeRTOS fixes that signature. Suppressing it needed `--inline-suppr` in `platformio.ini`'s `check_flags`.
