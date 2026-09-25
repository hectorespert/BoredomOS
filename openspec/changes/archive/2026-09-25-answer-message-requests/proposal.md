## Why

A ground station has no way to ask this firmware for a message it can already build, or
for the rate a message is sent at. `MAV_CMD_REQUEST_MESSAGE` (512) — the current
mechanism; `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` is its deprecated predecessor — and
`MAV_CMD_GET_MESSAGE_INTERVAL` (510) are answered `MAV_RESULT_UNSUPPORTED` today, even
though every sender they would need (`sendHeartbeat`, `sendSysStatus`, …) and the
per-port schedule table that holds every rate already exist in `src/mavlink.cpp`. The
ground can arm the housekeeping stream with `MAV_CMD_SET_MESSAGE_INTERVAL` but cannot ask
what is armed. `MISSION_REQUEST_LIST` falls to the outer `default` and is answered with a
`STATUSTEXT` warning on every request, when the honest answer — "this vehicle holds no
mission" — is a single `MISSION_COUNT`.

## What Changes

- `MAV_CMD_REQUEST_MESSAGE` (512) is answered for the message ids this firmware can
  build on demand: `HEARTBEAT` (0), `SYS_STATUS` (1), `SYSTEM_TIME` (2),
  `BATTERY_STATUS` (147), `AUTOPILOT_VERSION` (148) and the new `PROTOCOL_VERSION`
  (300). The requested message is sent once, on the port the request arrived on,
  followed by `COMMAND_ACK` / `MAV_RESULT_ACCEPTED`. Any other id, including
  `NAMED_VALUE_INT` (252), is answered `MAV_RESULT_DENIED`, the precedent
  `MAV_CMD_SET_MESSAGE_INTERVAL` already sets for an id it recognises but does not serve.
  `BATTERY_STATUS` in the reduced configuration, where there is no battery reading,
  is answered `MAV_RESULT_TEMPORARILY_REJECTED` rather than with a message full of zeros.
- `MAV_CMD_GET_MESSAGE_INTERVAL` (510) is answered with `COMMAND_ACK` and a
  `MESSAGE_INTERVAL` (244) carrying the cadence that message actually has **on the port
  that asked**, read from that port's schedule entry rather than from a second copy of
  the numbers. `interval_us` follows the field's own definition: the interval when the
  message is streaming, `-1` when it is not (an armed-off housekeeping stream, or a
  message that only goes out on request), `0` for an id this firmware cannot emit.
- `PROTOCOL_VERSION` (300) is a new outbound message, constant like
  `AUTOPILOT_VERSION`, sent only when requested.
- `MISSION_REQUEST_LIST` (43) is answered `MISSION_COUNT` (44) with `count = 0`, echoing
  the request's `mission_type`, addressed back to the requester. This is an answer, not
  a mission protocol: no mission item message is handled and no capability bit changes.

Not in scope: `PARAM_REQUEST_LIST`. The parameter protocol has no way to say "zero
parameters" — `PARAM_VALUE` carries a parameter — and MAVProxy re-issues its fetch for as
long as it holds none, so an empty answer cannot exist; the case stays deliberately empty
until *Implement the MAVLink parameter protocol* is picked up. `REQUEST_DATA_STREAM` and
`FILE_TRANSFER_PROTOCOL` likewise stay reserved.

**MAVLink surface.** New outbound message ids: 244 `MESSAGE_INTERVAL`, 300
`PROTOCOL_VERSION`, 44 `MISSION_COUNT`. Newly answered commands: 510 and 512. No new
periodic stream and no changed rate — all three new messages leave only in reply to a
request, so the cadences and the write queues' depth argument (`src/main.cpp`) are
untouched. The identity triple (system `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`)
does not change.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `mavlink-link`: gains requirements that a requested message is sent on demand, that the
  interval of a message is reported truthfully, that `PROTOCOL_VERSION` is answered, and
  that a mission list request is answered with an empty mission.

## Impact

- **RAM.** No task, queue or library is added, so the rules for those do not apply, but
  the figure is read, not assumed: a build of the current tree reports
  `committed 29728 B of 32768 (90.7%)`, `headroom 3040 B (minimum 1024)` from
  `scripts/ram_budget.py`. The three new `LinkMsgKind` variants carry 8 B
  (`MESSAGE_INTERVAL`) and 3 B (`MISSION_COUNT`) or nothing (`PROTOCOL_VERSION`); the
  `LinkMsg` union is already sized by `STATUSTEXT` and its `static_assert` (≤ 64 B) holds,
  so no queue item grows and no queue costs more. The id → sender table is `const`, so it
  lands in flash. What can move is `TaskMavlink`'s stack (384 words): it gains the decode
  locals for `COMMAND_LONG`'s two new branches and for `MISSION_REQUEST_LIST`, and its
  high-water mark has to be read again after the change — the build cannot tell.
- **Code.** `src/mavlink.cpp` (the table, two senders, the new `case`/`else if` branches,
  `mavlinkPack` cases) and `include/LinkMsg.h` (three `LinkMsgKind` values and their
  payload structs). No new file owns a resource; the schedule table stays where it is,
  which is what lets `GET_MESSAGE_INTERVAL` read it without a second owner.
- **Other active changes.** None: `openspec list` is empty, so no claim of another change
  is invalidated. `openspec/specs/memory-budget` still describes the retired heap model;
  this change touches no queue memory and does not carry the delta that fixes it.
- **`ARCHITECTURE.md`.** The `Mavlink` bullet in §6 (which lists the cases of the outer
  switch and the `COMMAND_LONG` sub-switch, and says an unrecognised `msgid` produces a
  `STATUSTEXT`) is updated in the same commit.
- **Tests.** A new HIL module for the four behaviours; it needs the board. No existing HIL
  case names commands 510 or 512 — `check_unsupported_commands.py` uses
  `MAV_CMD_DO_SET_MODE` as its unrecognised command — so none is invalidated. The scenarios
  are exercised by HIL only; nothing in `test_libs` reaches `src/`. **Four scenarios were
  written and never run**, because they need the board latched into the reduced configuration
  or an adapter on D0/D1, neither of which was available: `BATTERY_STATUS` requested with no
  battery reading, `BATTERY_STATUS` in the reduced configuration reporting no stream,
  a request on one port not answering on the other, and the housekeeping interval reported per
  port. In the spec they read as contract; they are hoped-for, not proven.
- **`TODO.md`.** *Answer the GCS messages that are ignored today* is only partly picked
  up, so, following `answer-autopilot-version-requests` and
  `answer-unsupported-command-long-requests`, it is annotated and trimmed to what is still
  open (`REQUEST_DATA_STREAM`, `PARAM_REQUEST_LIST`, FTP, and which commands beyond these
  deserve real support), not deleted; its "telemetry rates from the ground" question is
  narrowed by 510. *Review the contents of the messages already emitted* names
  `MISSION_REQUEST_LIST` among the messages that reach the `default` `STATUSTEXT`; that
  example is corrected.
