## Context

See `proposal.md` — Why. The relevant current state, read from the tree rather than
recalled:

- `TaskMavlink` (`src/mavlink.cpp`) already round-robins a `ScheduleEntry` table —
  `{ function, interval_ms, last_ms, enabled }` — checking every entry once per wake
  and calling every one that is due, in the same pass. `sendHeartbeat` and
  `sendSystemTime` are always enabled; `sendBatteryStatus` is gated
  `!reducedConfiguration` because it depends on the battery sense, which the reduced
  configuration avoids touching.
- `serialWriteQueue` has a fixed depth of 4 (`src/main.cpp:319`). `ARCHITECTURE.md:216`
  derives that depth from three send paths that could hold one item at the same
  instant — heartbeat, battery status and a `TIMESYNC` reply — plus one. `CLAUDE.md`
  requires re-deriving that backing — depth plus one block per producer that can hold
  an unsent item, one per consumer holding an unreleased one — for any change to the
  depth or the set of producers.
- `TaskMavlink`'s `COMMAND_LONG` switch answers `MAV_CMD_GET_HOME_POSITION` (silently)
  and `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` (with `COMMAND_ACK`); every other command,
  including `MAV_CMD_SET_MESSAGE_INTERVAL`, falls through unanswered.
- **Task creation is conditional.** `src/main.cpp:335-357`: `taskSerialReadHandler`,
  `taskSerialWriteHandler`, `taskMavlinkHandler` and `taskCliHandler` are created in
  every configuration. `taskLoggerHandler` and `taskSdWriteHandler` are created only
  `if (!reducedConfiguration) { if (sdCardAvailable) { ... } }` — in the reduced
  configuration, or with no SD card, both handles stay `NULL`. FreeRTOS's
  `uxTaskGetStackHighWaterMark(xTask)` treats `xTask == NULL` as "the calling task",
  not an error — calling it with either of these handles when unset would silently
  report `TaskMavlink`'s own high-water mark under the wrong name, not a missing value.
- **The idle task is not reachable without a build flag this firmware doesn't set.**
  `xTaskGetIdleTaskHandle()` compiles only under `INCLUDE_xTaskGetIdleTaskHandle`,
  which is absent from `platformio.ini`'s `build_flags` (only
  `INCLUDE_uxTaskGetStackHighWaterMark` is set). `src/hooks.cpp`'s idle task memory is
  file-local static storage with no handle exposed. Adding the flag is a build-level
  decision with its own RAM justification, not a free extension of this change's
  table.
- **There are six application tasks, not eight.** `src/cli.cpp`'s comment sizing its
  `ps` snapshot buffer ("9 tasks exist today (8 ours, idle)") predates
  `fold-periodic-telemetry-into-mavlink-task`, which removed `TaskHeartbeat` and
  `TaskMavlinkBatteryStatus` as separate tasks; `src/main.cpp` creates exactly six
  (`SerialRead`, `SerialWrite`, `Mavlink`, `Cli`, `Logger`, `SdWrite`). That comment is
  stale and out of this change's scope to fix.
- **`MAV_CMD_SET_MESSAGE_INTERVAL`'s interval parameter is microseconds, not
  milliseconds.** `MESSAGE_INTERVAL` (244), the message this command configures,
  carries `int32_t interval_us`; `param2` of the command uses the same unit. A 1 Hz
  request arrives as `1000000.0f`, which must be converted before it can be compared
  against or stored in `ScheduleEntry.interval_ms`.
- `src/cli.cpp`'s `ps` gets a full `TaskStatus_t` per task via `uxTaskGetSystemState()`
  into a static 432 B buffer, because it also needs to print priority and state. This
  change only needs the stack high-water mark, which `uxTaskGetStackHighWaterMark()`
  returns directly from a `TaskHandle_t` — the 432 B buffer has no counterpart here.

## Goals / Non-Goals

**Goals:**

- Deliver exactly what `specs/mavlink-link/spec.md`'s new requirement describes:
  off by default, armed by `MAV_CMD_SET_MESSAGE_INTERVAL`, one message per schedule
  pass, session-scoped.
- Reuse the schedule table and the queue exactly as they exist, with the one
  depth change this section justifies explicitly — no new task, no new build flag.

**Non-Goals:**

- Persisting the armed state — `proposal.md` and the spec both rule this out.
- Publishing on the USB endpoint `add-usb-dual-protocol` introduces. That change gives
  each port its own schedule table by design ("two independent links, not a mirror");
  extending housekeeping to USB is a follow-up once that lands, not part of this
  change. This does mean a ground station connected only over USB, after that change
  lands, cannot reach this telemetry until that follow-up exists — the motivation in
  `proposal.md` is about the UART link surviving the USB mode switch, which this
  change satisfies on its own; USB-side housekeeping is separately scoped.
- Including the FreeRTOS idle task in the published set (see Context).
- Disambiguating truncated task names beyond what `ps` already accepts (see Risks).

## Decisions

### One value per schedule pass, with its own round-robin index — not a burst function

**Alternative rejected: one `ScheduleEntry` function that sends all N values when
due.** `TaskMavlink` and `TaskSerialWrite` run at the same priority
(`PRIORITY_HIGH`), and `configUSE_TIME_SLICING` only yields between them at a tick
boundary, not mid-loop. A function sending several messages back-to-back risks
filling `serialWriteQueue` and dropping the rest — not a race, a deterministic loss
given the queue depth and the priority tie.

**Chosen:** the new entry carries its own `static uint8_t` cursor and fires exactly
one send per due check, advancing the cursor and wrapping after the last live value.
This makes it structurally identical to every existing entry from the queue's point
of view — at most one unsent item per entry per pass.

### The published set is dynamic: 2 heap values plus whichever task handles are non-NULL

Not 11, and not a fixed 8. The table built in `src/mavlink.cpp` holds all six task
handles (`taskSerialReadHandler` through `taskSdWriteHandler`), but the cursor skips
any handle that is `NULL` at the moment it would be read rather than calling
`uxTaskGetStackHighWaterMark()` on it — see Context for why that call is unsafe on a
`NULL` handle. The practical set is 8 values (2 heap + 6 tasks) in the normal
configuration with an SD card, and 6 values (2 heap + 4 tasks) in the reduced
configuration or with no SD card. `specs/mavlink-link/spec.md`'s scenario for the
reduced configuration is written against this: arming succeeds identically, the
published set is smaller because two tasks do not exist, not because anything is
withheld.

**Alternative rejected: build the table once at boot from whichever handles are
non-NULL then.** Rejected because it is no simpler — the handles are already known
statically, checking `!= NULL` per read costs one comparison, and a boot-time table
would still need the same check if a handle could ever transition (it cannot today,
which is exactly why the per-read check is cheap: dead code the moment task creation
becomes dynamic is a smaller risk than a stale table).

### The idle task is left out entirely

**Alternative rejected: add `INCLUDE_xTaskGetIdleTaskHandle=1` and publish its stack
mark too.** This is a real build flag change, not a free extension — `CLAUDE.md`
requires any RAM-relevant build flag to carry its own justification, and the idle
task's stack is FreeRTOS's own fixed-size (`configMINIMAL_STACK_SIZE`) bookkeeping
allocation, not an application task whose sizing this project tunes. Diagnostic value
is low next to the cost of adding a new build flag for it. Left out; six application
tasks only.

### `NAMED_VALUE_INT` only

Every value here — bytes of heap, words of stack — is an integer.
`NAMED_VALUE_FLOAT` (251), which the superseded `TODO.md` entry also named, has no
value to carry and is left out. One message type, not two.

### Converting the command's interval, and rejecting one that is too fast

`param2` arrives in microseconds (see Context). The handler converts to milliseconds
before writing `ScheduleEntry.interval_ms`, and rejects — with `COMMAND_ACK` /
`MAV_RESULT_DENIED`, not a silent clamp — any positive value below **1000 ms**.

The floor exists for two independent reasons, not one:

- **The cadence guarantee.** `mavlink-link`'s existing requirement 4 says transmitting
  frames must not shift the firmware's periodic cadences. A housekeeping interval far
  below the existing 1 Hz/0.5 Hz cadences would put a CPU-time cost (packing and
  writing a frame) on the schedule far more often than anything else does today,
  which is the kind of accumulation that requirement rules out.
- **The queue's spare capacity.** `serialWriteQueue`'s depth (see the next decision)
  is sized for every schedule entry that could coincide on the same pass. A very
  short housekeeping interval does not, by itself, cause more than one housekeeping
  send per pass — the round-robin cursor already guarantees that — but it does raise
  how often housekeeping's due tick lands on the same pass as the others, which is
  the scenario the depth change below is sized against. A floor at 1000 ms keeps that
  coincidence no more likely than the existing entries already make it.

A silent clamp was considered and rejected: a ground station requesting 10 ms and
observing 1000 ms without being told would read as the firmware ignoring the
request, not honouring a floor.

### `serialWriteQueue`'s depth grows from 4 to 5

**Re-deriving the backing, per `CLAUDE.md`:** today's depth 4 covers three paths that
could hold one item at the same instant — `sendHeartbeat`, `sendBatteryStatus` and a
`TIMESYNC` reply (`ARCHITECTURE.md:216`) — plus one. Housekeeping adds a fourth
schedule entry that can independently be due on the same pass as the other three:
with `sendHeartbeat` and `sendSystemTime` at 1 Hz staggered 500 ms apart,
`sendBatteryStatus` at 2 Hz, and housekeeping at 1000 ms or slower, all four periods
are independent (not harmonics of one another by construction), so a pass where all
four are simultaneously due, and a `TIMESYNC` reply arrives in that same pass, is not
excluded by anything in the schedule. That is five items wanting the queue in one
pass. Depth 4 would drop one of them, deterministically, on that specific
coincidence, exactly the failure mode the previous version of this design ruled out
for a single entry's own burst but did not check across entries.

**Cost:** measured after implementation at 4 B, not the 291 B first estimated here —
`proposal.md`'s Impact section explains the gap (task 6.1). The queue's own storage
array grows by one `mavlink_message_t*` slot (4 B, `.bss`); the extra in-flight
`mavlink_message_t` this depth allows for draws on `configTOTAL_HEAP_SIZE`'s
existing 936 B of slack (`ARCHITECTURE.md` §4) rather than growing that fixed-size
array, so it costs FreeRTOS heap headroom, not `.bss`/`.data`.

**Alternative rejected: stagger housekeeping's phase to never coincide with the
other three.** Possible for as long as the other three keep exactly their current
periods, but it makes the queue's safety depend on four independently-owned
schedules never being retimed without someone re-checking this coincidence — a
future change to `sendBatteryStatus`'s rate would silently reopen the drop. Paying
291 B once is cheaper than that standing obligation.

### No persistence, and no gate on the reduced configuration — see proposal.md

Both are design consequences already reasoned through in `proposal.md` — What
Changes; repeated here only as pointers so this section stays about *how*, not *why*:
session-scoped state lives in the same `.bss` cursor and `enabled` flag as any other
schedule entry, with nothing written to non-volatile storage, and the schedule
entry's `enabled` default requires no `reducedConfiguration` branch because arming it
depends only on a `COMMAND_LONG` arriving and being acted on, which `fault-recovery`
already guarantees in both configurations. What *does* change between configurations
is the published set's size, covered above.

### Where the new code lives

The new `ScheduleEntry` and the new `COMMAND_LONG` case go into `src/mavlink.cpp` as
it exists today — the raw `switch` and the `schedule[]` array, not a
`mavlinkHandleInbound()` seam, because that seam does not exist yet
(`add-usb-dual-protocol` proposes it but has not landed). See proposal.md's Impact
for the ordering note if that change is picked up first.

## Risks / Trade-offs

- **`"SerialWrite"` truncates to `"SerialWrit"` in the 10-byte `name` field.** No other
  task name collides with the truncated form today, and `cli.cpp`'s own `ps <name>`
  already truncates its argument the same way to match against
  `configMAX_TASK_NAME_LEN`, so this is an existing acceptance, not a new one. →
  Worth a one-line note in `ARCHITECTURE.md` or a code comment where the table is
  built, so a future task name is not chosen without checking for a collision in the
  truncated form.
- **A ground station requests housekeeping and then the radio link drops before
  disabling it.** Nothing distinguishes "link temporarily unreachable" from "ground
  station wants it forever" — the entry stays enabled, sending into a queue that may
  or may not be draining, until the next reset. This is the same trade-off
  `SET_MESSAGE_INTERVAL` carries for every other MAVLink vehicle; no timeout is added
  here, matching the ecosystem convention documented in `proposal.md`.
- **The cycle length grows every time a task is added.** Nothing in this design
  bounds it, and a future task means one more entry in the round-robin, and one more
  potential simultaneous-due coincidence with the other schedule entries, without
  anyone deciding that was fine. → Worth a comment at the task table noting that
  housekeeping's cycle length and the `serialWriteQueue` depth derivation both follow
  the task count, so both are visible next to the place a new task gets added.
- **Two independent tables of the same six handles.** `src/cli.cpp`'s `ps` and this
  change's round-robin both need to know which tasks exist, one via
  `uxTaskGetSystemState()`, one via a static handle table. They can drift if a task is
  added to one and not the other. → Not unified here: `cli.cpp` needs name, priority
  and state and this does not, so a shared table would carry fields one side never
  uses. Left as parallel structures, flagged for whoever adds the next task.
