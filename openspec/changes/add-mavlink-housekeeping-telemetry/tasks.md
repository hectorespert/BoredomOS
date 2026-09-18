## 1. Round-robin data source

- [ ] 1.1 Add a small static table pairing each existing task handle
  (`taskSerialReadHandler`, `taskSerialWriteHandler`, `taskMavlinkHandler`,
  `taskCliHandler`, `taskLoggerHandler`, `taskSdWriteHandler`, plus the idle task) with
  its `NAMED_VALUE_INT` name, and a cursor covering heap_free, heap_min, then each
  table entry. Verify with `pio run` on all three environments — CI-checkable, no
  board needed.
- [ ] 1.2 Add the function that reads the value the cursor currently points at
  (`xPortGetFreeHeapSize()`, `xPortGetMinimumEverFreeHeapSize()`, or
  `uxTaskGetStackHighWaterMark()` on the current handle) and advances the cursor,
  wrapping after the last task. Verify with `pio run`.
- [ ] 1.3 Add the `NAMED_VALUE_INT` send, following the existing `sendHeartbeat` /
  `sendSystemTime` shape: `pvPortMalloc`, `NULL` check, `mavlink_msg_named_value_int_pack`,
  `xQueueSend` onto `serialWriteQueue`, `vPortFree` on a failed send. Verify with
  `pio run`.

## 2. Schedule integration

- [ ] 2.1 Add the new `ScheduleEntry` to `TaskMavlink`'s `schedule[]`, `enabled: false`
  at start, calling the function from 1.3. Verify with `pio run` and `pio check`.
- [ ] 2.2 With the board flashed and no ground station having sent
  `MAV_CMD_SET_MESSAGE_INTERVAL`, confirm no `NAMED_VALUE_INT` appears on the link —
  needs the assembled board and a MAVLink capture (MAVProxy or `pymavlink`) watching
  the link.

## 3. Ground-configurable rate

- [ ] 3.1 Add a `MAV_CMD_SET_MESSAGE_INTERVAL` case to `TaskMavlink`'s `COMMAND_LONG`
  switch: decode `param1` (message id) and `param2` (interval); when `param1 == 252`,
  `param2 == -1` disables the schedule entry, `param2 == 0` leaves it disabled (the
  default rate is off), `param2 > 0` sets `interval_ms` and enables it. Reply
  `COMMAND_ACK` with `MAV_RESULT_ACCEPTED` in every case that reaches this branch.
  Verify with `pio run`.
- [ ] 3.2 With the board flashed, send `MAV_CMD_SET_MESSAGE_INTERVAL(252, <positive
  interval>)` and confirm both the `COMMAND_ACK` and the first `NAMED_VALUE_INT`
  arrive — needs the assembled board and a MAVLink client that can send commands
  (`pymavlink`).
- [ ] 3.3 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, -1)` while armed and confirm the
  stream stops after the `COMMAND_ACK` — needs the assembled board.
- [ ] 3.4 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 0)` from a clean boot and confirm no
  `NAMED_VALUE_INT` starts, only the `COMMAND_ACK` — needs the assembled board.

## 4. HIL suite (`test/test_hil/`)

- [ ] 4.1 Add the four scenarios from section 3 as HIL cases — extending
  `check_telemetry.py` or a new module, whichever keeps one capability's cases
  together per its existing convention. Needs the assembled board; run via
  `pio test -e bench` so the link is reachable over USB without a UART adapter.
- [ ] 4.2 Add a case: arm housekeeping, reset the board (a 1200-baud touch or the
  RESET button, per how the suite already resets between cases), and confirm no
  `NAMED_VALUE_INT` arrives after the reset until requested again. Needs the
  assembled board and hands on the reset — no script here presses it unattended.
- [ ] 4.3 Add a case exercising the reduced configuration (no SD card, no RTC — reusing
  `check_recovery.py`'s existing setup for entering it) and confirming
  `MAV_CMD_SET_MESSAGE_INTERVAL(252, ...)` is still acknowledged and still starts the
  stream. Needs the assembled board and hands to pull the card/clock per the existing
  degraded-mode setup.

## 5. RAM and stack verification

- [ ] 5.1 Run `pio run` fresh on this tree after the above and compare the reported
  headroom against the ~56 B estimate in `proposal.md`'s Impact table; correct the
  proposal if the measured figure differs. CI-checkable, no board needed for the
  build itself, but the number must come from an actual build, not the estimate.
- [ ] 5.2 Flash the board, arm housekeeping, let it run for several full cycles, then
  read `TaskMavlink`'s stack high-water mark from the SD housekeeping log (or via
  the `ps Mavlink` console command) and confirm it still fits its 256-word stack with
  margin. Needs the assembled board — this is the check that a build alone cannot
  perform, per `CLAUDE.md`'s rule that a task body change is not verified by
  compiling it.

## 6. Documentation

- [ ] 6.1 Add a short comment at the task-handle table from 1.1 noting that
  `"SerialWrite"` truncates to `"SerialWrit"` in the 10-byte `NAMED_VALUE_INT` name
  field, so a future task name should be checked against the truncated form before
  reuse. No board needed.
- [ ] 6.2 Add a short comment near `src/main.cpp`'s task creation calls noting that the
  housekeeping round-robin's cycle length follows the task count, so a new task
  lengthens it without anyone deciding that on purpose. No board needed.
- [ ] 6.3 Check whether `CLAUDE.md`'s MAVLink command-handling notes need a line about
  this change's `MAV_CMD_SET_MESSAGE_INTERVAL` handling; update if so. No board
  needed.
