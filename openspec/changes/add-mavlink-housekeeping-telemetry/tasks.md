## 1. Round-robin data source

- [ ] 1.1 Add a static table of the six existing task handles
  (`taskSerialReadHandler`, `taskSerialWriteHandler`, `taskMavlinkHandler`,
  `taskCliHandler`, `taskLoggerHandler`, `taskSdWriteHandler` — no idle task; see
  `design.md`) paired with each one's `NAMED_VALUE_INT` name, and a cursor covering
  heap_free, heap_min, then each table entry. Verify with `pio run` on all three
  environments — CI-checkable, no board needed.
- [ ] 1.2 Add the function that reads the value the cursor currently points at
  (`xPortGetFreeHeapSize()`, `xPortGetMinimumEverFreeHeapSize()`, or
  `uxTaskGetStackHighWaterMark()` on the current handle) and advances the cursor,
  wrapping after the last table entry. **The handle for the current cursor position
  MUST be checked for `NULL` before calling `uxTaskGetStackHighWaterMark()`** —
  `taskLoggerHandler`/`taskSdWriteHandler` stay `NULL` in the reduced configuration or
  with no SD card (`src/main.cpp:350-357`), and FreeRTOS treats a `NULL` handle as
  "the calling task", not an error, so an unguarded call would silently report
  `TaskMavlink`'s own stack mark under the wrong task's name. A `NULL` entry is
  skipped and the cursor advances again in the same pass rather than sending a value
  for it. Verify with `pio run`, then with the board flashed in the reduced
  configuration (no SD card) confirm only the four live tasks' names ever appear —
  needs the assembled board.
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
  switch: decode `param1` (message id) and `param2` — **`param2` is microseconds**
  (matching `MESSAGE_INTERVAL.interval_us`, not `ScheduleEntry.interval_ms` directly)
  — convert to milliseconds before comparing or storing it. When `param1 == 252`:
  `param2 == -1` disables the schedule entry; `param2 == 0` disables it too and
  leaves/sets it disabled regardless of whether it was already running (the default
  rate is off, in both the clean-boot and the already-armed case); a converted value
  at or above 1000 ms sets `interval_ms` and enables the entry; a converted value
  greater than zero but below 1000 ms changes nothing and is answered
  `MAV_RESULT_DENIED`, not clamped. Reply `COMMAND_ACK` in every case that reaches
  this branch (`MAV_RESULT_ACCEPTED` except the denied case above). Verify with
  `pio run`.
- [ ] 3.2 With the board flashed, send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 1000000)`
  (1000 ms in microseconds) and confirm both the `COMMAND_ACK`/`ACCEPTED` and the
  first `NAMED_VALUE_INT` arrive roughly 1 s later — needs the assembled board and a
  MAVLink client that can send commands (`pymavlink`).
- [ ] 3.3 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, -1)` while armed and confirm the
  stream stops after the `COMMAND_ACK` — needs the assembled board.
- [ ] 3.4 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 0)` from a clean boot and confirm no
  `NAMED_VALUE_INT` starts, only the `COMMAND_ACK`; then arm with 3.2's command and
  send `param2 == 0` again, confirming the already-running stream stops — needs the
  assembled board.
- [ ] 3.5 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 500000)` (500 ms — below the floor)
  and confirm `COMMAND_ACK`/`MAV_RESULT_DENIED`, and that publishing state is
  unchanged (still off if it was off, still at its previous interval if it was on) —
  needs the assembled board.

## 4. `serialWriteQueue` depth

- [ ] 4.1 Raise `serialWriteQueue`'s depth from 4 to 5 in `src/main.cpp` (the
  `xQueueCreateStatic` call and its backing storage array), re-deriving the depth per
  `CLAUDE.md`'s rule and `design.md`'s "serialWriteQueue's depth grows from 4 to 5"
  decision. Verify with `pio run` and the RAM check in section 6.

## 5. HIL suite (`test/test_hil/`)

- [ ] 5.1 Add the scenarios from section 3 as HIL cases — extending
  `check_telemetry.py` or a new module, whichever keeps one capability's cases
  together per its existing convention. **Order matters**: `run.py` gives every case
  in a run the same `Link` with no reset between them
  (`test/test_hil/run.py:8-10`), and this stream is session-scoped, so a case that
  arms housekeeping leaves it armed for every case that runs after it in the same
  invocation. Structure the new cases so every one that asserts "no `NAMED_VALUE_INT`"
  runs before any that arm it, and have the last arming case explicitly disable the
  stream (`param2 == -1`) at its end, restoring a clean state for whatever runs after.
  Needs the assembled board; run via `pio test -e bench` so the link is reachable
  over USB without a UART adapter.
- [ ] 5.2 Strengthen the armed-case assertion beyond "one `COMMAND_ACK` and one
  `NAMED_VALUE_INT` arrived": capture a full cycle's worth of messages and assert the
  name/value sequence matches the expected set for the board's current configuration
  (heap_free, heap_min, then each live task in the table order from 1.1), and that
  `HEARTBEAT`/`SYSTEM_TIME`/`BATTERY_STATUS` keep arriving at their existing rates
  while the stream is enabled. An implementation that repeats one value, uses the
  wrong units, or never reaches a task boundary must fail this, not just the
  first-message check. Needs the assembled board.
- [ ] 5.3 Add a case: arm housekeeping, reset the board with the **RESET button**
  (needs hands — **do not use a 1200-baud touch**: `test/test_hil/README.md:86-87`
  states this board enters DFU and stays there on a 1200-baud touch, it does not
  time out back into the sketch the way AVR boards do, and using it here would strand
  the board and require a manual reflash), and confirm no `NAMED_VALUE_INT` arrives
  after the reset until requested again. Needs the assembled board and hands on the
  reset — no script here presses it unattended.
- [ ] 5.4 Add a case exercising the reduced configuration. **`check_recovery.py` does
  not create that configuration** — its existing cases only assert against a board
  that is already reduced, and self-skip (`NoLinkError`) otherwise
  (`test/test_hil/check_recovery.py:45-53`) — so this needs its own documented manual
  precondition (per `fault-recovery`: three consecutive failed boots, or the SD card
  physically removed, depending on which reduced-entry path is being exercised) rather
  than assuming a fixture that does not exist. Document the precondition and the
  restore-to-normal step in the case itself. Confirm
  `MAV_CMD_SET_MESSAGE_INTERVAL(252, ...)` is still acknowledged and still starts the
  stream, with the smaller (6-value) set from 1.1/1.2. Needs the assembled board and
  hands to set up and then clear the reduced configuration.

## 6. RAM and stack verification

- [ ] 6.1 Run `pio run` fresh on this tree after the above and compare the reported
  headroom against the ~337 B estimate in `proposal.md`'s Impact table; correct the
  proposal if the measured figure differs. CI-checkable, no board needed for the
  build itself, but the number must come from an actual build, not the estimate.
- [ ] 6.2 Flash the board, arm housekeeping, let it run for several full cycles, then
  read `TaskMavlink`'s stack high-water mark from the SD housekeeping log (or via
  the `ps Mavlink` console command) and confirm it still fits its 256-word stack with
  margin. Needs the assembled board — this is the check that a build alone cannot
  perform, per `CLAUDE.md`'s rule that a task body change is not verified by
  compiling it.

## 7. Documentation

- [ ] 7.1 Correct `ARCHITECTURE.md:482-489`'s RAM figures to match a fresh build on
  this tree (see `proposal.md`'s Impact section for why they currently disagree), and
  update its `TaskMavlink` description (currently: three fixed sends, awake at least
  once a second) to state the fourth, ground-selected schedule entry and its 1000 ms
  floor. Required in the same commit per `CLAUDE.md:107`. No board needed beyond the
  `pio run` in 6.1 that produces the figures.
- [ ] 7.2 Add a short comment at the task-handle table from 1.1 noting that
  `"SerialWrite"` truncates to `"SerialWrit"` in the 10-byte `NAMED_VALUE_INT` name
  field, so a future task name should be checked against the truncated form before
  reuse. No board needed.
- [ ] 7.3 Add a short comment near `src/main.cpp`'s task creation calls noting that
  the housekeeping round-robin's cycle length and the `serialWriteQueue` depth
  derivation (section 4) both follow the task count, so a new task should check both
  before being added. No board needed.
- [ ] 7.4 Check whether `CLAUDE.md`'s MAVLink command-handling notes need a line about
  this change's `MAV_CMD_SET_MESSAGE_INTERVAL` handling; update if so. No board
  needed.
