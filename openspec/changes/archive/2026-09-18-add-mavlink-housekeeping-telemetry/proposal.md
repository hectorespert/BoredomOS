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
change — read fresh via `pio run` on this tree, reproduced twice before this change
was implemented, and again after): before, `committed 29084 B of 32768, headroom
3684 B`; after implementing every task in `tasks.md` that does not need the
assembled board, `committed 29092 B of 32768, headroom 3676 B` (minimum floor
1024 B). **The measured delta is 8 B, not the ~337 B this section originally
estimated** — task 6.1 caught the gap; see the corrected table and explanation
below.

**This conflicted with `ARCHITECTURE.md:482-489`**, which stated `30140 B` committed
and `2628 B` headroom, last updated at `9483ebe` (`configUSE_TIME_SLICING` landing).
The two disagreed by exactly 1056 B in both directions, consistent with something
freeing RAM after that commit without `ARCHITECTURE.md` being updated — not with
either number being read wrong. `add-usb-dual-protocol`'s own proposal, written
independently after that commit, also cited 3684 B. `ARCHITECTURE.md` now carries
this change's own post-implementation figures instead (task 7.1), so the two no
longer disagree.

| Item | Estimated | Measured | Where |
|---|---|---|---|
| New task | none | none | — |
| New queue | none | none | — |
| `serialWriteQueue` depth: 4 -> 5 | 291 B (one more possible in-flight `mavlink_message_t`) | **4 B** (one more pointer slot in `.bss`) | see below |
| Round-robin state: a cursor plus a table of the six existing task handles, skipping any that are `NULL` | ~30 B | **~1-4 B** (just the cursor; the table is `const`) | `.bss` |
| One more `ScheduleEntry` in `TaskMavlink`'s schedule array | ~16 B | **0 B** (`TaskMavlink`'s own stack, not `.bss`/`.data`) | `TaskMavlink`'s stack |
| | **~337 B of 3684 B headroom** | **8 B of 3684 B headroom** | |

**Why the estimate overstated it by roughly 40x.** Two things in the original
estimate conflated *capacity* with *footprint*:

- The task-name table (`kNameHeapFree` through `kNameSdWrite`, and the table of
  handle-pointer/name pairs) is declared `const`. The compiler places `const` data
  in flash (`.rodata`), not RAM — the ~30 B estimate assumed `.bss` the way a
  mutable table would need, but nothing here is mutated after it is built.
- The 291 B "cost" of the queue depth change was never a `.bss`/`.data` allocation
  to begin with — it was FreeRTOS heap *capacity* (`configTOTAL_HEAP_SIZE`, a fixed
  `0x1800`-byte array already reserved in `.bss` before this change). Raising the
  queue's depth means more of that already-reserved 6136 B usable capacity can be
  in use at once (5200 B worst case before this change, 5504 B after — see
  `ARCHITECTURE.md` §4), not that the array itself grows. The existing 936 B of
  slack in that capacity absorbs the increase without `configTOTAL_HEAP_SIZE`
  needing to change; only the queue's own pointer-slot array (`sizeof(void*)` per
  extra depth, 4 B on this 32-bit target) is new `.bss`.

The real cost is one pointer in `.bss` (the queue storage array) plus a
single-byte cursor — `TaskMavlink`'s own stack, not global RAM, absorbs the fourth
schedule entry, and its high-water mark against the 256-word budget is what task
6.2 checks on the board, which a RAM figure cannot substitute for.

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
of them is picked up for implementation, not discovered mid-edit. Now that this
change has landed, `add-usb-dual-protocol`'s own RAM ledger should be computed
against the measured baseline above (`29092 B` committed, `3676 B` headroom), not
against the `3684 B` figure its current proposal cites — an 8 B difference, not
the ~337 B this section originally estimated before implementation corrected it.

`TODO.md`'s *"Publish housekeeping live with `NAMED_VALUE_INT` / `NAMED_VALUE_FLOAT"*
entry is deleted in the same commit as this proposal.
