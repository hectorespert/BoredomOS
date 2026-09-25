## Context

`TaskMavlink` (`src/mavlink.cpp`) owns the protocol. Its `COMMAND_LONG` sub-switch is an
`if`/`else if` chain on `command.command`; anything it does not recognise is answered
`MAV_RESULT_UNSUPPORTED`, so 510 and 512 reach that default today. The per-port
`ScheduleEntry schedule[kPortCount][5]` is a local of the task body, rows `{HEARTBEAT,
SYSTEM_TIME, BATTERY_STATUS, housekeeping, SYS_STATUS}`, each carrying its sender, its
`interval_ms` and an `enabled` flag; the housekeeping row is the only one a ground
station can change (`MAV_CMD_SET_MESSAGE_INTERVAL`, 252 only), and the battery row is
built `!reducedConfiguration`. Every sender has the signature `void(uint8_t port)` and
enqueues a `LinkMsg` by value on that port's write queue, drop-on-full. `mavlinkPack`, in
the same file, turns a `LinkMsg` into a frame.

`AUTOPILOT_VERSION` already has an on-request path (`MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`,
`answer-autopilot-version-requests`) that sends the message first and the ACK second.
See `proposal.md` for why any of this is wanted.

## Goals / Non-Goals

**Goals:**

- One place that says which message ids the firmware can provide on request and which of
  them stream, so `REQUEST_MESSAGE` and `GET_MESSAGE_INTERVAL` cannot drift from each other
  or from the schedule.
- A reported interval that cannot disagree with what is on the wire.

**Non-Goals:**

- Making any rate adjustable beyond what `SET_MESSAGE_INTERVAL` allows today, or serving
  more ids on request than the six named. Adding a message to the served set later is one
  table row and one sender.
- Touching the outer `default` that answers an unrecognised `msgid` with a `STATUSTEXT`;
  that belongs to *Review the contents of the messages already emitted*. This change only
  moves `MISSION_REQUEST_LIST` out of it.
- Refactoring `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`. It keeps its own branch and its own
  order (message, then ACK); `REQUEST_MESSAGE` for id 148 reaches the same sender.

## Decisions

### One `const` table from message id to sender and schedule row

`src/mavlink.cpp` gains a `const` table of `{message id, one-shot sender, schedule row}`:

```
   id   sender               row         served by 512   GET reports
   0    sendHeartbeat        row 0       yes             row interval, or -1 if disabled
   1    sendSysStatus        row 4       yes             "
   2    sendSystemTime       row 1       yes             "
   147  sendBatteryStatus    row 2       yes*            "  (-1 in reduced)
   148  sendAutopilotVersion none        yes             -1
   300  sendProtocolVersion  none        yes             -1
   252  (housekeeping)       row 3       NO              armed interval, or -1
   any other id                          NO (DENIED)     0
                                         * TEMPORARILY_REJECTED when reducedConfiguration
```

The rows are named with constants beside the existing `kHousekeepingScheduleIndex`, so the
table indexes the schedule by name rather than by position. `GET_MESSAGE_INTERVAL` reads
`schedule[port][row]`: `enabled ? interval_ms * 1000 : -1`. Because the value is read from
the entry that drives the emission, it is the interval in use — including the truncation to
whole milliseconds a `SET_MESSAGE_INTERVAL` request goes through — and the disabled battery
row in the reduced configuration reports `-1` without a special case.

The sender for a scheduled row is the row's own function, so a one-shot `HEARTBEAT` is the
same code as a periodic one. Nothing else enters the schedule: the table is `const` and
lives in flash, and the schedule stays a five-row local, so the write queues' depth
argument (`src/main.cpp`, sized against five per-port producers coinciding) is not disturbed
by adding rows or entries.

*Alternatives.* A `switch` in each of the two handlers duplicates the id list and lets the
two drift. Walking `schedule[]` alone cannot serve 148 and 300, which have no row, and the
rows do not carry their ids.

### `MESSAGE_INTERVAL` semantics: `-1` for "streamable or requestable, but off", `0` for "not available"

The field's own definition is `-1` disabled, `0` not available, `> 0` the interval. This
firmware reads "available" as "the ground can obtain it by a stream or by
`REQUEST_MESSAGE`". So `AUTOPILOT_VERSION` and `PROTOCOL_VERSION`, which are requestable
but never stream, report `-1`, and a message the firmware also emits but only as a reply
(`MESSAGE_INTERVAL` itself, `MISSION_COUNT`, `COMMAND_ACK`, `STATUSTEXT`, `TIMESYNC`)
reports `0`: no request or stream can produce it, so it is not available.

For an id that is not in the table, `GET_MESSAGE_INTERVAL` is answered `ACCEPTED` plus
`interval_us = 0`, not `DENIED`. The protocol defines `0` for exactly that and a client
enumerating ids gets a uniform answer; `DENIED` is kept for a parameter that is not an id
at all.

### Reply order is ACK first, for both commands

`MAV_CMD_GET_MESSAGE_INTERVAL`'s definition says "the receiver should ACK the command and
then emit its response in a `MESSAGE_INTERVAL`". `MAV_CMD_REQUEST_MESSAGE`'s says nothing
about order, so it takes the same order rather than a second convention in one `else if`
chain. This differs deliberately from the `AUTOPILOT_VERSION` path, which sends its
data first so a client that does not wait for the ACK still has it
(`answer-autopilot-version-requests`); that path is unchanged and is not folded into the
table's dispatch. Both replies are non-blocking and drop-on-full, as every reply in this
file is, so an ACCEPTED ACK does not guarantee the message survived a full queue — the same
weakness the 520 path has, and the drop is already counted in `SYS_STATUS.errors_count1`.

### Result codes

- An id not in the served set: `DENIED`, matching the precedent `SET_MESSAGE_INTERVAL` sets
  for an id it recognises the command for but does not serve.
- Id 252 on `REQUEST_MESSAGE`: `DENIED`. Housekeeping sends one value per pass and keeps a
  round-robin position per port; a one-shot would advance that position as a side effect, so
  "the message" is not something that can be sent once.
- `BATTERY_STATUS` in the reduced configuration: `TEMPORARILY_REJECTED`. The condition is a
  state that can clear (the once-only retry reboot), which is that code's definition, and it
  is the reason the row is disabled there. *Alternative:* `FAILED`, rejected because it
  reads as an error executing the command rather than a state.
- The message-id parameter arrives as a float. The existing `SET_MESSAGE_INTERVAL` branch
  casts it straight to `uint16_t`, which turns 65538 into 2. The two new branches check that
  it is a whole number in 0–65535 before narrowing and answer `DENIED` otherwise (NaN fails
  every comparison, and a fractional value is refused rather than truncated). The existing
  branch is not changed by this design.

### Three new `LinkMsgKind` values, packed where the rest are

`MessageInterval {uint16_t id; int32_t interval_us}`, `MissionCount {uint8_t
target_system, target_component, mission_type}` and `ProtocolVersion` (no payload: every
field is a constant, like `AutopilotVersion`). `mavlinkPack` builds them with the
`_pack_chan` form on the port's channel, like every other case. `MISSION_COUNT.count` is
`0` and `opaque_id` is `0`, which the field defines as "plan ids are not supported".
`PROTOCOL_VERSION` reports 200/200/200, since the firmware transmits MAVLink 2 only, and
passes a `static const` zero array for both hashes so that nothing is built on the stack.
The union stays under its 64 B assertion by a wide margin (largest new payload: 8 B).

### `MISSION_REQUEST_LIST` gets its own outer `case`

It decodes the request, and enqueues `MissionCount` addressed to `msg.sysid` /
`msg.compid` with the request's `mission_type`. The firmware performs no target-system
filtering anywhere else and this does not add it; `REQUEST_MESSAGE`'s target-address
parameter is ignored for the same reason, and the reply goes to the port it came from,
which is what the existing per-port requirement already says.

### Ownership

`src/mavlink.cpp` owns everything touched: the table, the senders, the schedule and the
reads of it. The handlers run in `TaskMavlink`, the schedule's only user, so reading the
row for `GET_MESSAGE_INTERVAL` needs no second owner and no lock. `reducedConfiguration` is
read the way the rest of the file already reads it. No peripheral is reached.

## Risks / Trade-offs

- **`TaskMavlink`'s stack is 384 words and tuned tight.** The new branches add small
  decoded locals; the build cannot say whether that fits. → Read `TaskMavlink`'s
  high-water mark from the housekeeping stream on the board after the change and compare
  with a stream taken before it.
- **Request flood.** Each request enqueues two frames, like 520 already does, and a peer
  can repeat it as fast as the link allows, filling that port's write queue and dropping
  periodic frames on that port only. → Inherited from every `COMMAND_LONG`, not introduced;
  drops show in `errors_count1` and the other port is unaffected. No rate limit is added.
- **A "requestable" claim that reduced mode cannot honour.** Only `BATTERY_STATUS`
  depends on the configuration, handled by the code above. → The reduced configuration is
  reachable on the bench only by hand (see `CLAUDE.md`); the scenario is marked as needing
  it in `tasks.md`.
- **The `AUTOPILOT_VERSION` spec text says the mission protocol is "not implemented
  today".** It stays true — no mission item message is handled — but a reader could take
  `MISSION_COUNT` for a contradiction. → The new requirement says outright that answering
  the list request is not implementing the protocol and keeps the capability bits clear, so
  the existing requirement is not modified.
- **Reported interval differs from the requested one by truncation.** A request for
  1500500 µs runs at 1500 ms and is reported as 1500000. → Intended: it reports what runs.

## Migration Plan

None: no persisted state, no wire change to any existing message. Rollback is reverting the
commit; a ground station that never sends 510, 512 or `MISSION_REQUEST_LIST` sees nothing
different.
