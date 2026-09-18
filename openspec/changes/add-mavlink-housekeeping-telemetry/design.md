## Context

See `proposal.md` — Why. The relevant current state, read from the tree rather than
recalled:

- `TaskMavlink` (`src/mavlink.cpp`) already round-robins a `ScheduleEntry` table —
  `{ function, interval_ms, last_ms, enabled }` — checking every entry once per wake
  and calling every one that is due, in the same pass. `sendHeartbeat` and
  `sendSystemTime` are always enabled; `sendBatteryStatus` is gated
  `!reducedConfiguration` because it depends on the battery sense, which the reduced
  configuration avoids touching.
- `serialWriteQueue` has a fixed depth of 4 (`src/main.cpp:319`), backed for the
  producers that exist today, each of which sends at most one message per entry per
  wake. `CLAUDE.md` requires re-deriving that backing — depth plus one block per
  producer holding an unsent item, one per consumer holding an unreleased one — for
  any change to the depth or the set of producers.
- `TaskMavlink`'s `COMMAND_LONG` switch answers `MAV_CMD_GET_HOME_POSITION` (silently)
  and `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` (with `COMMAND_ACK`); every other command,
  including `MAV_CMD_SET_MESSAGE_INTERVAL`, falls through unanswered.
- `src/cli.cpp`'s `ps` gets a full `TaskStatus_t` per task via `uxTaskGetSystemState()`
  into a static 432 B buffer, because it also needs to print priority and state. This
  change only needs the stack high-water mark, which `uxTaskGetStackHighWaterMark()`
  returns directly from a `TaskHandle_t` — the 432 B buffer has no counterpart here.
- Task names are fixed C strings passed to `xTaskCreateStatic` (`src/main.cpp:335`
  onward): `"SerialRead"`, `"SerialWrite"`, `"Mavlink"`, `"Cli"`, `"Logger"`,
  `"SdWrite"`, plus FreeRTOS's own `"IDLE"` task and this firmware's `"Mavlink"`-family
  helpers. `NAMED_VALUE_INT`'s `name` field is 10 bytes; `"SerialWrite"` is 11.

## Goals / Non-Goals

**Goals:**

- Deliver exactly what `specs/mavlink-link/spec.md`'s new requirement describes:
  off by default, armed by `MAV_CMD_SET_MESSAGE_INTERVAL`, one message per schedule
  pass, session-scoped.
- Reuse the schedule table and the queue exactly as they exist — no new task, no new
  queue, no depth change.

**Non-Goals:**

- Persisting the armed state — `proposal.md` and the spec both rule this out.
- Publishing on the USB endpoint `add-usb-dual-protocol` introduces. That change gives
  each port its own schedule table by design ("two independent links, not a mirror");
  extending housekeeping to USB is a follow-up once that lands, not part of this
  change.
- Disambiguating truncated task names beyond what `ps` already accepts (see Risks).

## Decisions

### One value per schedule pass, with its own round-robin index — not a burst function

**Alternative rejected: one `ScheduleEntry` function that sends all 11 values when
due.** `TaskMavlink` and `TaskSerialWrite` run at the same priority
(`PRIORITY_HIGH`), and `configUSE_TIME_SLICING` only yields between them at a tick
boundary, not mid-loop. A function sending 11 messages back-to-back would fill the
4-slot `serialWriteQueue` on the fourth send and drop the remaining seven, every
time — not a race, a deterministic loss given the queue depth and the priority tie.

**Alternative rejected: raise `serialWriteQueue`'s depth to absorb the burst.**
Backing 11 in-flight `mavlink_message_t` items costs 11 x 291 B = 3201 B, against a
current build headroom of 3684 B (`pio run`, this tree) — more than half the entire
heap, permanently, for every message that ever passes through the queue, to buy
nothing a diagnostic channel needs: nobody requires all 11 values to land within the
same tick.

**Chosen:** the new entry carries its own `static uint8_t` cursor and fires exactly
one send per due check, advancing the cursor and wrapping after the last task. This
makes it structurally identical to every existing entry from the queue's point of
view — at most one unsent item at a time — so no backing arithmetic changes.

### Read stack marks directly, not through a `TaskStatus_t` snapshot

`uxTaskGetStackHighWaterMark(TaskHandle_t)` returns exactly the one field this needs.
The alternative — reusing `cli.cpp`'s `uxTaskGetSystemState()` pattern — pulls in name,
priority and state for every task into a 432 B static buffer `ps` needs and this does
not. A small static table of the existing task handles (`taskSerialReadHandler`,
`taskMavlinkHandler`, etc., already declared `extern` where needed) plus the cursor is
the entire new state this adds.

### `NAMED_VALUE_INT` only

Every value here — bytes of heap, words of stack — is an integer.
`NAMED_VALUE_FLOAT` (251), which the superseded `TODO.md` entry also named, has no
value to carry and is left out. One message type, not two.

### Arming through `MAV_CMD_SET_MESSAGE_INTERVAL`, not a new command

`MAV_CMD_SET_MESSAGE_INTERVAL` (511) already means "control the rate of a message id"
to every MAVLink ground station, and its own documented semantics for `param2 == 0`
— "request default rate (which may be zero)" — already say exactly what this change
needs "off by default" to mean, without the firmware inventing a private reading of
the field. A new `MAV_CMD_USER_*` command was considered and rejected: it would need
a GCS operator to know a satellite-specific command id, where `SET_MESSAGE_INTERVAL`
already works from MAVProxy's and QGroundControl's stock rate-control UI.

### No persistence, and no gate on the reduced configuration — see proposal.md

Both are design consequences already reasoned through in `proposal.md` — What
Changes; repeated here only as pointers so this section stays about *how*, not *why*:
session-scoped state lives in the same `.bss` cursor and `enabled` flag as any other
schedule entry, with nothing written to non-volatile storage, and the schedule
entry's `enabled` default requires no `reducedConfiguration` branch because arming it
depends only on a `COMMAND_LONG` arriving and being acted on, which `fault-recovery`
already guarantees in both configurations.

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
- **The cycle length (11 values today) grows every time a task is added.** Nothing in
  this design bounds it, and a future task means one more entry in the round-robin
  without anyone deciding that was fine. → Worth a comment at the task table noting
  that housekeeping's cycle length is derived from the task count, so it is visible
  next to the place a new task gets added.
