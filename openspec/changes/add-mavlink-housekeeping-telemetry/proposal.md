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
  heap, then each task's high-water mark, one message per schedule tick, cycling back
  to the start after the last task. Never more than one `NAMED_VALUE_INT` in flight at
  a time — the same shape every existing entry in `TaskMavlink`'s schedule table
  already has, so `serialWriteQueue` needs no depth change and no new backing.
- **Off by default, armed by the ground.** The new schedule entry starts
  `enabled: false`. `TaskMavlink`'s `COMMAND_LONG` switch gains a case for
  `MAV_CMD_SET_MESSAGE_INTERVAL` (511) targeting message id 252: `param2 == -1`
  disables it, `param2 == 0` asks for "the default rate" and that default is off
  (the command's own documented meaning of `0`, not a firmware-specific reading of
  it), `param2 > 0` sets the per-value interval and enables the entry. Every case
  replies with `COMMAND_ACK`. Nothing is sent unless a ground station has asked,
  matching the "console emits nothing unless spoken to" invariant `console-cli`
  already holds on the USB side.
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
change — read fresh via `pio run` on this tree): `committed 29084 B of 32768,
headroom 3684 B` (minimum floor 1024 B).

| Item | Cost | Where |
|---|---|---|
| New task | none | — |
| New queue | none | — |
| `serialWriteQueue` depth | unchanged (4) — this entry never holds more than one unsent item, the same as every existing schedule entry | — |
| Round-robin state: an index plus a small table of task handles to call `uxTaskGetStackHighWaterMark()` against (no `TaskStatus_t` snapshot needed — unlike `cli.cpp`'s `ps`, this only ever needs the stack mark, not name/priority/state, so it skips that struct's 432 B entirely) | ~40 B, estimate to be replaced by a measurement | `.bss` |
| One more `ScheduleEntry` in `TaskMavlink`'s existing schedule array | ~16 B | `TaskMavlink`'s own stack (256 words, already sized with headroom per `add-usb-dual-protocol`'s design notes) |
| | **~56 B of 3684 B headroom** | |

**Files.** Modified: `src/mavlink.cpp` (new schedule entry, new `COMMAND_LONG` case),
`CLAUDE.md` if the MAVLink command-handling convention needs updating,
`openspec/specs/mavlink-link/spec.md` (via this change's spec delta).

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
ledger is computed against by the ~56 B above; that change's arithmetic would need
re-checking against the new baseline, not reused from its current proposal.

`TODO.md`'s *"Publish housekeeping live with `NAMED_VALUE_INT` / `NAMED_VALUE_FLOAT"*
entry is deleted in the same commit as this proposal.
