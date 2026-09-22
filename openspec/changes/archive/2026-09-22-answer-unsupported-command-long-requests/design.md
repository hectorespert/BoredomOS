## Context

See proposal.md - Why. The relevant code is the `COMMAND_LONG` case in
`TaskMavlink`'s dispatch switch, `src/mavlink.cpp:757-838`. It is a sequence of
four independent `if (command.command == ...)` blocks, each `break`-ing out of
the outer `switch` on its own. None of them is an `else if`, and there is no
final `else`: a `command.command` that matches none of the four checks falls
through all of them and reaches the closing `break;` at line 838 having sent
nothing. `sendCommandAck(port, cmd, result)` already exists and is used by
three of the four handled commands; nothing new needs to be built to call it
from two more places.

## Goals / Non-Goals

**Goals:**
- Every `COMMAND_LONG`, handled or not, leaves this switch case having sent
  exactly one `COMMAND_ACK` for it.
- `MAV_CMD_SET_MESSAGE_INTERVAL` naming a message id this firmware does not
  publish leaves the same way, instead of silently doing nothing.

**Non-Goals:**
- Implementing real behaviour for any command beyond the four already handled.
  `MAV_RESULT_UNSUPPORTED` is the final word for all of them here; deciding
  which of them deserves more is the next entry in `TODO.md`, not this one.
- `PARAM_REQUEST_LIST`, `REQUEST_DATA_STREAM`, `FILE_TRANSFER_PROTOCOL`: these
  are different `msgid`s in the outer switch, not `COMMAND_LONG` sub-commands,
  and stay exactly as they are.

## Decisions

**The four `if` blocks become an `if` / `else if` / ... / `else` chain.**
Today they are independent statements, each ending in its own `break`, so
there is no single point to attach a terminal "none of the above" clause
without restructuring them. Converting to `else if` costs nothing behaviourally
— the four conditions are mutually exclusive on a single `uint16_t` equality
each, so no case that reaches one today can also reach another — and it turns
"falls through unanswered" into "provably reaches the final `else`", which is
the property the new requirement needs. The alternative, a `handled` boolean
set by each branch and checked after the chain, adds a variable to track what
the control flow already expresses once it is an `else if` chain, so it is not
used.

**The final `else` sends `MAV_RESULT_UNSUPPORTED`.** Per the
`MAV_RESULT` enum's own definitions (`common.xml`): `UNSUPPORTED` is "Command
is not supported (unknown)" — the firmware never reached any command-specific
logic — while `DENIED` is "supported but one or more parameter values are
invalid". A command this switch does not recognise at all is the first case,
not the second.

**`MAV_CMD_SET_MESSAGE_INTERVAL` naming an unpublished message id sends
`MAV_RESULT_DENIED`, not `UNSUPPORTED`.** Unlike the outer chain, this command
*is* recognised and partially valid — the firmware understands
`SET_MESSAGE_INTERVAL` as a command — it is the specific `msgid` parameter
that this firmware has no publisher for. That is exactly `DENIED`'s
definition, and matches the precedent already in this same handler: a rate
request below the 1000 ms floor is answered `DENIED`, not `UNSUPPORTED`,
for the identical reason (recognised command, rejected parameter value).
This mirrors the existing `if (requestedUs <= 0) {... } else { ... requestedMs
< 1000u ... DENIED ... }` structure one level up: both are "the command is
fine, this particular argument is not".

**`MAV_CMD_GET_HOME_POSITION` moves from its own dead-end `if` (checked,
never acknowledged) into the same `else if` chain, falling to the shared
`UNSUPPORTED` default.** It gets no bespoke handling because the firmware
holds no home position to report; keeping it as a distinguished branch that
sends the same `UNSUPPORTED` as the generic default would be a distinction
with no behavioural difference.

## Risks / Trade-offs

- [A `COMMAND_ACK` for a command the ground never intended as a real request
  (e.g. a GCS probing capabilities by sending several commands speculatively)
  now generates wire traffic that did not exist before] → Bounded: at most one
  `COMMAND_ACK` per inbound `COMMAND_LONG`, on the same non-blocking
  drop-on-full `xQueueSend` every other reply in this file already uses
  (`design.md` Decision 4 of `answer-autopilot-version-requests`), so a burst
  of unsupported commands degrades the same way a burst of supported ones
  already does — by dropping into a full queue — not by blocking or growing
  unbounded.
- [Converting four standalone `if`s into `else if` is a larger diff than
  adding one `else` at the end, and moves lines that reviewers must re-read
  even though their behaviour for the four existing conditions is unchanged]
  → Accepted: the alternative (a `handled` flag) touches the same four blocks
  anyway and adds a variable besides, so it is not a smaller diff, only a
  different one.
