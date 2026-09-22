## Why

`TaskMavlink`'s `COMMAND_LONG` sub-switch (`src/mavlink.cpp`) only answers four
commands. Every other `command.command` — including `MAV_CMD_GET_HOME_POSITION`,
which is checked but never acknowledged — falls through with no reply at all.
`MAV_CMD_SET_MESSAGE_INTERVAL` has the same gap one level down: it only recognises
`NAMED_VALUE_INT` (252) and silently drops a request naming any other message id.
A ground station has no way to tell "not supported" from "lost on the wire" and
keeps retrying forever — this is exactly the failure `ARCHITECTURE.md` already
documents as unresolved ("every other command it receives still falls through
unanswered").

## What Changes

- Every `COMMAND_LONG` whose `command` is not one of the four already handled
  (`MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`, `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`,
  `MAV_CMD_SET_MESSAGE_INTERVAL`, `MAV_CMD_GET_HOME_POSITION`) is answered with
  `COMMAND_ACK` / `MAV_RESULT_UNSUPPORTED`, on the port the request arrived on.
- `MAV_CMD_GET_HOME_POSITION` moves into that same default: it is checked today
  but never acknowledged, and the firmware implements no home position, so it is
  answered `MAV_RESULT_UNSUPPORTED` like any other command this firmware does not
  implement.
- `MAV_CMD_SET_MESSAGE_INTERVAL` targeting a message id other than
  `NAMED_VALUE_INT` (252) is answered `COMMAND_ACK` / `MAV_RESULT_DENIED` — the
  command itself is recognised and valid, only that message id is not one whose
  rate this firmware lets the ground adjust — instead of falling through
  unanswered as it does today.
- `PARAM_REQUEST_LIST`, `REQUEST_DATA_STREAM` and `FILE_TRANSFER_PROTOCOL` are
  untouched: they stay the reserved, deliberately-empty cases `ARCHITECTURE.md`
  already describes, for the parameter-protocol, telemetry-rate and FTP work
  `TODO.md` still lists separately.

Not in scope: which commands beyond the current four deserve real behaviour
(rather than a default `UNSUPPORTED`) is left for whoever picks that up next: this
change is the "always answer something" floor, not a survey of what else to
implement.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `mavlink-link`: every `COMMAND_LONG` now receives a `COMMAND_ACK` — either from
  existing per-command handling, or from one of the two defaults this change adds
  (`MAV_RESULT_UNSUPPORTED` for an unrecognised command, `MAV_RESULT_DENIED` for
  `MAV_CMD_SET_MESSAGE_INTERVAL` naming a message id this firmware does not let
  the ground adjust).

## Impact

- `src/mavlink.cpp`: the `COMMAND_LONG` sub-switch's `if` chain gains a final
  `else` branch sending `MAV_RESULT_UNSUPPORTED`, and the existing
  `MAV_CMD_SET_MESSAGE_INTERVAL` handler gains an `else` branch for a non-252
  message id sending `MAV_RESULT_DENIED`. Both reuse the existing
  `sendCommandAck()` — no new function, task, queue or library, so none of the
  RAM-cost rules for those apply.
- No new MAVLink message id, no new stream rate, and no change to the identity
  triple (system `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`): this only
  extends `COMMAND_ACK`, already sent by three other branches in this same
  switch, to two code paths that currently send nothing.
- `ARCHITECTURE.md` (around the `COMMAND_LONG` paragraph describing today's
  "every other command it receives still falls through unanswered") is updated
  in the same commit to describe the new default behaviour, per `CLAUDE.md`'s
  rule that a change making it inaccurate fixes it alongside the change.
- `test/test_hil/`: the closed-set assumption in whichever check enumerates
  expected `COMMAND_ACK` traffic (the pattern `check_recovery.py`'s task 7.5
  established for `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`) needs a case for an
  unrecognised command now producing `MAV_RESULT_UNSUPPORTED` instead of
  silence.
- `TODO.md`: this picks up only the "always answer something" floor from
  *Answer the GCS messages that are ignored today*. Following the precedent
  `answer-autopilot-version-requests` set for the same entry, the entry is
  **annotated, not deleted** — the parameter protocol, `REQUEST_DATA_STREAM`/
  telemetry-rate question, and "which commands deserve real support" are still
  open and still described there.
