## 1. Round-robin data source

- [x] 1.1 Add a static table of the six existing task handles
  (`taskSerialReadHandler`, `taskSerialWriteHandler`, `taskMavlinkHandler`,
  `taskCliHandler`, `taskLoggerHandler`, `taskSdWriteHandler` — no idle task; see
  `design.md`) paired with each one's `NAMED_VALUE_INT` name, and a cursor covering
  heap_free, heap_min, then each table entry. Verify with `pio run` on all three
  environments — CI-checkable, no board needed.
  Done: `src/mavlink.cpp`'s `kHousekeepingTasks` table and `housekeepingCursor`.
  `pio run` on `uno_r4_minima`, `bench` and `libs` all succeed.
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
  Code done: `nextHousekeepingValue()` in `src/mavlink.cpp` skips any `NULL`
  handle within the same call, per the design decision cited above. `pio run`
  passes. **Board confirmation in the reduced configuration is still
  outstanding** — not run, no board available in this session.
- [x] 1.3 Add the `NAMED_VALUE_INT` send, following the existing `sendHeartbeat` /
  `sendSystemTime` shape: `pvPortMalloc`, `NULL` check, `mavlink_msg_named_value_int_pack`,
  `xQueueSend` onto `serialWriteQueue`, `vPortFree` on a failed send. Verify with
  `pio run`.
  Done: `sendHousekeeping()` in `src/mavlink.cpp`. `pio run` succeeds.

## 2. Schedule integration

- [x] 2.1 Add the new `ScheduleEntry` to `TaskMavlink`'s `schedule[]`, `enabled: false`
  at start, calling the function from 1.3. Verify with `pio run` and `pio check`.
  Done: fourth entry in `TaskMavlink`'s `schedule[]`, `enabled: false`. `pio run`
  and `pio check` (cppcheck) both pass with no new findings beyond the existing
  style-only notes already present elsewhere in this file.
- [ ] 2.2 With the board flashed and no ground station having sent
  `MAV_CMD_SET_MESSAGE_INTERVAL`, confirm no `NAMED_VALUE_INT` appears on the link —
  needs the assembled board and a MAVLink capture (MAVProxy or `pymavlink`) watching
  the link.

## 3. Ground-configurable rate

- [x] 3.1 Add a `MAV_CMD_SET_MESSAGE_INTERVAL` case to `TaskMavlink`'s `COMMAND_LONG`
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
  Done: the `MAV_CMD_SET_MESSAGE_INTERVAL` block in `TaskMavlink`'s
  `COMMAND_LONG` case (`src/mavlink.cpp`). `pio run` succeeds.
- [ ] 3.2 With the board flashed, send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 1000000)`
  (1000 ms in microseconds) and confirm both the `COMMAND_ACK`/`ACCEPTED` and the
  first `NAMED_VALUE_INT` arrive roughly 1 s later — needs the assembled board and a
  MAVLink client that can send commands (`pymavlink`).
  Exercised by `test/test_hil/check_housekeeping.py`'s
  `test_arming_covers_the_full_cycle_without_disturbing_existing_telemetry`, not
  yet run against a board.
- [ ] 3.3 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, -1)` while armed and confirm the
  stream stops after the `COMMAND_ACK` — needs the assembled board.
  Exercised by `check_housekeeping.py`'s `test_disable_stops_an_armed_stream`, not
  yet run against a board.
- [ ] 3.4 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 0)` from a clean boot and confirm no
  `NAMED_VALUE_INT` starts, only the `COMMAND_ACK`; then arm with 3.2's command and
  send `param2 == 0` again, confirming the already-running stream stops — needs the
  assembled board.
  Exercised by `check_housekeeping.py`'s
  `test_default_rate_from_clean_boot_does_not_start_the_stream` and
  `test_default_rate_stops_an_already_armed_stream`, not yet run against a board.
- [ ] 3.5 Send `MAV_CMD_SET_MESSAGE_INTERVAL(252, 500000)` (500 ms — below the floor)
  and confirm `COMMAND_ACK`/`MAV_RESULT_DENIED`, and that publishing state is
  unchanged (still off if it was off, still at its previous interval if it was on) —
  needs the assembled board.
  Exercised by `check_housekeeping.py`'s `test_interval_below_the_floor_is_denied`,
  not yet run against a board.

## 4. `serialWriteQueue` depth

- [x] 4.1 Raise `serialWriteQueue`'s depth from 4 to 5 in `src/main.cpp` (the
  `xQueueCreateStatic` call and its backing storage array), re-deriving the depth per
  `CLAUDE.md`'s rule and `design.md`'s "serialWriteQueue's depth grows from 4 to 5"
  decision. Verify with `pio run` and the RAM check in section 6.
  Done: `src/main.cpp`'s `serialWriteQueueStorage` and `xQueueCreateStatic` call.
  `pio run` succeeds; RAM check in section 6 done (measured cost was 4 B for
  this specific change, far under the original 291 B estimate — see 6.1).

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
  Code done: `test/test_hil/check_housekeeping.py`, a new module (fits the
  existing one-file-per-topic convention better than folding into
  `check_telemetry.py`, which only covers always-on periodic telemetry). Cases
  ordered per the guidance above: silence and floor/default-rate checks first,
  then the full-cycle arm, then `param2 == 0` on an already-armed stream, then
  the explicit `-1` disable last, leaving a clean state. `python -c "import
  ast; ..."` and `run.py --list` both confirm the module loads and every case
  is discovered in the intended order; **not run against a board**.
- [ ] 5.2 Strengthen the armed-case assertion beyond "one `COMMAND_ACK` and one
  `NAMED_VALUE_INT` arrived": capture a full cycle's worth of messages and assert the
  name/value sequence matches the expected set for the board's current configuration
  (heap_free, heap_min, then each live task in the table order from 1.1), and that
  `HEARTBEAT`/`SYSTEM_TIME`/`BATTERY_STATUS` keep arriving at their existing rates
  while the stream is enabled. An implementation that repeats one value, uses the
  wrong units, or never reaches a task boundary must fail this, not just the
  first-message check. Needs the assembled board.
  Code done:
  `check_housekeeping.py`'s
  `test_arming_covers_the_full_cycle_without_disturbing_existing_telemetry`
  checks the full name/value cycle order against the configuration read from
  the current `HEARTBEAT`, and checks `HEARTBEAT`/`SYSTEM_TIME`/(when
  normal) `BATTERY_STATUS` rates from a fresh listen window while armed, not
  the run-wide cached sample. Not yet run against a board.
- [ ] 5.3 Add a case: arm housekeeping, reset the board with the **RESET button**
  (needs hands — **do not use a 1200-baud touch**: `test/test_hil/README.md:86-87`
  states this board enters DFU and stays there on a 1200-baud touch, it does not
  time out back into the sketch the way AVR boards do, and using it here would strand
  the board and require a manual reflash), and confirm no `NAMED_VALUE_INT` arrives
  after the reset until requested again. Needs the assembled board and hands on the
  reset — no script here presses it unattended.
  Code done: `check_housekeeping.py`'s `test_no_housekeeping_survives_a_reset`.
  Self-skips (`NoLinkError`) unless `HIL_MANUAL_RESET=1` is set, since `run.py`
  otherwise expects every case to finish unattended; when opted in it prompts
  for the physical reset and blocks on `input()` for confirmation. Not yet run
  against a board.
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
  Code done: `check_housekeeping.py`'s
  `test_housekeeping_in_the_reduced_configuration`, self-skipping
  (`NoLinkError`) unless the next `HEARTBEAT` already reports the reduced
  state — same convention as `check_recovery.py`'s state-decode cases. The
  precondition (remove the SD card, reset; reinsert and reset again to
  restore) and the smaller expected set are documented in its docstring. Not
  yet run against a board.

## 6. RAM and stack verification

- [x] 6.1 Run `pio run` fresh on this tree after the above and compare the reported
  headroom against the ~337 B estimate in `proposal.md`'s Impact table; correct the
  proposal if the measured figure differs. CI-checkable, no board needed for the
  build itself, but the number must come from an actual build, not the estimate.
  Done: measured `committed 29092 B, headroom 3676 B` — an 8 B delta from the
  pre-change baseline, not ~337 B. `proposal.md`'s Impact table and `design.md`'s
  queue-depth decision are both corrected, with the reason for the gap
  explained (the task table is `const`, in flash, not `.bss`; the queue's
  "cost" draws on existing FreeRTOS heap capacity slack rather than growing
  `.bss`/`.data`).
- [ ] 6.2 Flash the board, arm housekeeping, let it run for several full cycles, then
  read `TaskMavlink`'s stack high-water mark from the SD housekeeping log (or via
  the `ps Mavlink` console command) and confirm it still fits its 256-word stack with
  margin. Needs the assembled board — this is the check that a build alone cannot
  perform, per `CLAUDE.md`'s rule that a task body change is not verified by
  compiling it.

## 7. Documentation

- [x] 7.1 Correct `ARCHITECTURE.md:482-489`'s RAM figures to match a fresh build on
  this tree (see `proposal.md`'s Impact section for why they currently disagree), and
  update its `TaskMavlink` description (currently: three fixed sends, awake at least
  once a second) to state the fourth, ground-selected schedule entry and its 1000 ms
  floor. Required in the same commit per `CLAUDE.md:107`. No board needed beyond the
  `pio run` in 6.1 that produces the figures.
  Done: §4's queue table and its backing explanation (depth 5, 7 blocks, 5504 B
  total), §5.1's `TaskMavlink` description (the fourth entry, its floor, and its
  session-scoped arming), and §8's committed/headroom figures are all updated to
  the measured build.
- [x] 7.2 Add a short comment at the task-handle table from 1.1 noting that
  `"SerialWrite"` truncates to `"SerialWrit"` in the 10-byte `NAMED_VALUE_INT` name
  field, so a future task name should be checked against the truncated form before
  reuse. No board needed.
  Done: comment above `kNameHeapFree`..`kNameSdWrite` in `src/mavlink.cpp`.
- [x] 7.3 Add a short comment near `src/main.cpp`'s task creation calls noting that
  the housekeeping round-robin's cycle length and the `serialWriteQueue` depth
  derivation (section 4) both follow the task count, so a new task should check both
  before being added. No board needed.
  Done: comment above the `xTaskCreateStatic` calls in `src/main.cpp`'s `setup()`.
- [x] 7.4 Check whether `CLAUDE.md`'s MAVLink command-handling notes need a line about
  this change's `MAV_CMD_SET_MESSAGE_INTERVAL` handling; update if so. No board
  needed.
  Checked: `CLAUDE.md`'s only MAVLink-specific convention is the identity-triple
  rule ("Every outbound MAVLink message uses the same identity triple"), which
  this change already follows (`sendHousekeeping()` packs with `1`,
  `MAV_COMP_ID_AUTOPILOT1`, same as every other send in `src/mavlink.cpp`) — no
  separate command-handling convention exists there to extend. No update made.
  Separately, and out of this task's scope: `CLAUDE.md`'s `test/test_hil/`
  bullet ("Nine cases across four `check_*.py` modules") was already stale
  before this change (the real pre-change count was 14 cases across 6
  modules, per `ARCHITECTURE.md`'s own count) and is now further out of date
  since this change adds an eighth module and eight more cases; left alone as
  a pre-existing inaccuracy outside "MAVLink command-handling," not
  introduced by this task.
