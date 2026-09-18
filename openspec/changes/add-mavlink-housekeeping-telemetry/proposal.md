## Why

The housekeeping data that sizes this firmware's stacks — free heap and each task's
high-water mark — can only be read two ways today: pulling the SD card to parse the
`.mpk` log, or reaching the USB text console's `ps`/`free` commands. Both require
physical access to the board. Once `add-usb-dual-protocol` lands, the second option
stops being reliably available even on the bench: its one-way mode switch means the
first MAVLink frame on USB permanently hands that port to the link, and the CLI needs
a reset to come back. A ground station connected over MAVLink has no equivalent to
`ps`/`free` at all, on either port, today or after that change.

`TODO.md`'s *"Publish housekeeping live with `NAMED_VALUE_INT` / `NAMED_VALUE_FLOAT`"*
proposed the messages already. This change gives that idea the two things it was
missing: a reason tied to an actual gap (diagnostics surviving the USB mode switch,
not just "nicer than pulling the card"), and a rate the ground controls rather than
one hard-coded into the firmware.

## What Changes

- **A new outbound message the firmware has never sent: `NAMED_VALUE_INT` (252).**
  Every housekeeping value here is an integer — free heap and minimum-ever-free heap
  in bytes, each task's stack high-water mark in words — so `NAMED_VALUE_FLOAT` (251),
  which the TODO entry also named, is not needed.
- **One new value per `TaskMavlink` pass, round-robin.** Free heap, minimum-ever-free
  heap, then the stack high-water mark of every task that exists in the running
  configuration, one message per schedule tick, cycling back to the start after the
  last one. Never more than one `NAMED_VALUE_INT` in flight at a time — the same
  shape every existing entry in `TaskMavlink`'s schedule table already has. The set
  is 8 values (2 heap + 6 tasks) with an SD card in the normal configuration, and 6
  (2 heap + 4 tasks) in the reduced configuration or with no SD card, since
  `taskLoggerHandler`/`taskSdWriteHandler` are only created otherwise
  (`src/main.cpp:350-357`) — a task that does not exist is skipped, not reported
  under the wrong name. The FreeRTOS idle task is left out; see `design.md`.
- **Off by default, armed by the ground, with a floor on the interval.** The new
  schedule entry starts `enabled: false`. `TaskMavlink`'s `COMMAND_LONG` switch gains
  a case for `MAV_CMD_SET_MESSAGE_INTERVAL` (511) targeting message id 252: `param2`
  (microseconds, per the command's own field) is converted to milliseconds before
  use. `-1` disables it, `0` asks for "the default rate" and that default is off (the
  command's own documented meaning of `0`, not a firmware-specific reading of it,
  and this stops an already-running stream too), a value at or above 1000 ms sets the
  interval and enables the entry, and a positive value below 1000 ms is refused —
  `COMMAND_ACK` / `MAV_RESULT_DENIED`, no silent clamp — both to hold the link's
  existing cadence guarantee and to bound how often housekeeping can coincide with
  the other three schedule entries in the same pass (see `design.md`'s queue-depth
  decision). Every case replies with `COMMAND_ACK`. Nothing is sent unless a ground
  station has asked, matching the "console emits nothing unless spoken to" invariant
  `console-cli` already holds on the USB side.
- **`serialWriteQueue` grows from depth 4 to 5.** A fourth independently-clocked
  schedule entry can be due on the same pass as the three that already justify depth
  4 (`ARCHITECTURE.md:216`); one more slot covers that coincidence. Re-derived, not
  assumed — see `design.md`.
- **Session-scoped, not persisted.** Rebooting returns the entry to disabled. This
  matches how `SET_MESSAGE_INTERVAL` is treated across the MAVLink ecosystem — a
  ground station re-requests the rates it wants on each connection, the vehicle does
  not remember them — and keeps this change independent of the (unimplemented)
  MAVLink parameter protocol. It also means a fault-triggered reboot never comes back
  chattering telemetry nobody has asked for in that session, which matters because
  the next point removes any gate that would otherwise stop it from mattering.
- **No gate on the reduced configuration.** None of this data touches the SD card,
  the real-time clock or the battery sense, so `fault-recovery`'s "reduced
  configuration stays reachable and commandable" already covers it — a ground
  station can arm housekeeping in the reduced configuration exactly as it can in the
  normal one, without this change adding a special case for it.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `mavlink-link`: gains a requirement that the link can be asked, via
  `MAV_CMD_SET_MESSAGE_INTERVAL`, to publish `NAMED_VALUE_INT` housekeeping values —
  off by default, session-scoped, paced to one message per schedule pass — and that
  doing so does not shift the existing periodic cadences the capability already
  guarantees (requirement 4: transmitting frames must not shift housekeeping
  sampling or telemetry rates). No change to the identity triple, the link port, or
  any existing requirement's wording beyond adding this one.

## Impact

**RAM**, against the current build's own reported headroom (not quoted from another
change — read fresh via `pio run` on this tree, reproduced twice): `committed
29084 B of 32768, headroom 3684 B` (minimum floor 1024 B).

**This conflicts with `ARCHITECTURE.md:482-489`**, which states `30140 B` committed
and `2628 B` headroom, last updated at `9483ebe` (`configUSE_TIME_SLICING` landing).
The two disagree by exactly 1056 B in both directions, which is consistent with
something freeing RAM after that commit without `ARCHITECTURE.md` being updated —
not with either number being read wrong. `add-usb-dual-protocol`'s own proposal,
written independently after that commit, also cites 3684 B. This change uses the
freshly measured figure, per `CLAUDE.md`'s "read fresh, never quote" rule, and
Impact/Files below adds correcting `ARCHITECTURE.md`'s stale figure as part of this
change, so the two stop disagreeing.

| Item | Cost | Where |
|---|---|---|
| New task | none | — |
| New queue | none | — |
| `serialWriteQueue` depth: 4 -> 5 | 291 B (one more possible in-flight `mavlink_message_t`) | FreeRTOS heap, worst case |
| Round-robin state: a cursor plus a table of the six existing task handles to call `uxTaskGetStackHighWaterMark()` against, skipping any that are `NULL` (no `TaskStatus_t` snapshot needed — unlike `cli.cpp`'s `ps`, this only ever needs the stack mark, not name/priority/state, so it skips that struct's 432 B entirely) | ~30 B, estimate to be replaced by a measurement | `.bss` |
| One more `ScheduleEntry` in `TaskMavlink`'s existing schedule array | ~16 B | `TaskMavlink`'s own stack (256 words, already sized with headroom per `add-usb-dual-protocol`'s design notes) |
| | **~337 B of 3684 B headroom** | |

**Files.** Modified: `src/mavlink.cpp` (new schedule entry, new `COMMAND_LONG` case),
`src/main.cpp` (`serialWriteQueue`'s depth constant, and the two Risk comments
`design.md` calls for near the task-creation calls), `ARCHITECTURE.md` (its stale RAM
figures corrected to a fresh build's, and its `TaskMavlink` contract — three fixed
sends, awake at least once a second — updated for the fourth, ground-selected entry;
`CLAUDE.md:107` requires this in the same commit), `CLAUDE.md` if the MAVLink
command-handling convention needs updating, `openspec/specs/mavlink-link/spec.md`
(via this change's spec delta), `test/test_hil/` (new or extended cases, see
`tasks.md`).

**This alters the MAVLink surface**, per the rule requiring that be stated
explicitly: no new message id is added to the dialect (`NAMED_VALUE_INT` and
`MAV_CMD_SET_MESSAGE_INTERVAL` already exist in `common`, already linked in via
`MAVLink.h`), but the firmware emits a message type it never has before, and acts on
a command (`MAV_CMD_SET_MESSAGE_INTERVAL`) it previously only ignored. No change to
the identity triple, the link port, or any existing message's rate.

**Interaction with `add-usb-dual-protocol` (active, not yet applied).** Both changes
touch `src/mavlink.cpp`. That change's design refactors `TaskMavlink` into a thin
loop over a shared `mavlinkHandleInbound()` and moves message construction behind
`include/Mavlink.h`; this change adds a schedule entry and a `COMMAND_LONG` case to
the structure as it exists today. Whichever lands second has to carry its addition
into whatever shape the first one left behind — this change does not depend on that
one, and either order is workable, but the order should be a decision made when one
of them is picked up for implementation, not discovered mid-edit. If this change
lands first, it also shrinks the 3684 B headroom `add-usb-dual-protocol`'s own RAM
ledger is computed against by the ~337 B above; that change's arithmetic would need
re-checking against the new baseline, not reused from its current proposal.

`TODO.md`'s *"Publish housekeeping live with `NAMED_VALUE_INT` / `NAMED_VALUE_FLOAT"*
entry is deleted in the same commit as this proposal.
