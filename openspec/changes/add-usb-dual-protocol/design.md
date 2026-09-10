## Context

See `proposal.md` — Why. This change stacks on `add-console-cli`, which must land
first: it creates the console task, the port owner and the command set that this one
teaches to speak a second protocol.

What madflight actually does, read from `src/cli/cli.cpp` and
`src/rcl/RclGizmoMavlink.cpp` rather than assumed:

- **Mode detection** (`Cli::update_MODE_CLI`): every inbound byte is offered to an MSP
  parser and a MAVLink parser before being appended to the command line. The MAVLink
  parser is instantiated lazily on seeing `0xFD` or `0xFE`, but the mode only changes
  once `process_char` reports a **complete, framed message** — a header byte alone
  never switches. Once switched, `cli_mode` is never set back to `MODE_CLI`.
- **Transmit** (`RclGizmoMavlink::telem_send`): packs into a stack
  `uint8_t buf[MAVLINK_MAX_PACKET_LEN]`, takes a TX mutex with a timeout of 0 ms for
  telemetry, checks `availableForWrite() >= len`, and writes or gives up. Nothing is
  allocated and nothing waits.
- **Schedule** (`telem_update`): a `telem_sched[]` table of `{function, interval_ms,
  last_ms}`, round-robined **one message per pass**, breaking out of the loop as soon
  as a send fails so a full buffer costs one attempt, not a spin.
- **Order** (`RclGizmoMavlink::update`): all pending input is consumed before any
  telemetry is emitted, with the comment "don't fill up txbuf space with telemetry if
  we need to send a reply".

Facts about this target that shape the port:

- `SerialUSB` implements `availableForWrite()`, so the non-blocking check is available.
  Whether it reports a useful number is a board question, not a source question, and
  is in the task list.
- Our largest periodic frame is `BATTERY_STATUS` at roughly 54 bytes on the wire, well
  under any plausible CDC buffer, so `availableForWrite() >= len` will not deadlock the
  way it could for a 280-byte frame on a small buffer.
- `src/serial.cpp` already keeps a `uint8_t buf[MAVLINK_MAX_PACKET_LEN]` on a 192-word
  task stack, so the stack shape this design needs is already proven on this hardware.

## Goals / Non-Goals

**Goals:**

- One cable on the bench does both jobs, chosen by what you point at it rather than by
  what you flashed.
- The radio link is not touched. `Serial1` keeps its queues, its tasks and its
  behaviour byte for byte.
- Delete a build configuration instead of adding one.

**Non-Goals:**

- MSP. madflight's third mode has no consumer here.
- Returning to CLI mode without a reset.
- Routing between the ports. A frame arriving on one is acted on by the firmware, not
  forwarded to the other; this is not a MAVLink router.
- Per-port message selection or `REQUEST_DATA_STREAM` handling. Both ports emit the
  same fixed set.

## Decisions

### Switch on a checksum-valid frame, never on a header byte

`0xFD` is an ordinary byte. A terminal, a stray paste or line noise will produce it,
and because the switch is one-way, treating it as the trigger would mean a single
corrupt byte permanently costs the CLI until someone power-cycles the satellite.

Requiring a complete frame that passes CRC makes the trigger something no accident
produces. This is what madflight does — it instantiates the parser on the header byte
but switches on a framed message — and it is worth copying precisely.

### The switch is one-way

Chosen over an inactivity timeout and over an escape sequence. The alternatives each
add a state to specify, a state to test, and a way for the port to be in the mode you
did not expect. One-way means the port's mode is a pure function of "has a valid frame
arrived since reset", which is one line to reason about and one line to test.

The cost is real and is not hidden: after a ground station has spoken, the CLI needs a
reset. On a bench that is a button. This is acceptable *because* it is the bench port;
it would not be acceptable for a flight-critical control path, and this is not one.

### The USB transmit path does not use the queue

Every other outbound message in this firmware is `pvPortMalloc`ed, pushed to
`serialWriteQueue` as a pointer, and freed by `TaskSerialWrite`. The USB endpoint
deliberately does not join that pipeline.

*Alternative rejected:* a second queue with its own drain task. It would cost a task
(4 x stack + 112 B), a queue, and — the real objection — roughly 291 bytes of heap per
in-flight message, because `mavlink_message_t` is 291 bytes to carry an 18-byte
payload. Two independent streams through that pipeline is where the "independent
links" idea gets expensive.

*Alternative rejected:* reusing `serialWriteQueue` and having `TaskSerialWrite` write
each message to both ports. That is the mirror the change was explicitly asked not to
be, and it couples the radio's drain rate to a USB host that may have stopped reading.

madflight's shape avoids both: pack on the stack, check for room, write or drop. The
heap cost of the second endpoint is zero and the only new cost is stack, which is the
resource we have most of.

The consequence to accept honestly: **the USB stream is lossy by design.** A host that
stops reading loses frames rather than causing back-pressure. For a bench diagnostic
that is the correct trade, and the spec states it as a requirement rather than leaving
it as an implementation accident.

### The schedule lives in the console task, not in the existing producers

`TaskHeartbeat` and `TaskMavlinkBatteryStatus` produce for the queue that feeds
`Serial1`. They are left alone. The console task gains its own `{function, interval,
last}` table and its own round-robin pass, so the two streams are independent in
exactly the way that was asked for: separate schedules, separate sequence numbers,
separate failure behaviour.

It also keeps ownership clean. The console task owns the USB port; it is the only
thing that writes to it, in either mode. No mutex is needed, which is the same reason
the rest of the firmware needs none — and it is why madflight's `tx_mux` has no
counterpart here. madflight needs a mutex because several of its tasks share one port;
we have one owner per port and intend to keep it.

### The message logic is shared with `src/mavlink.cpp`, not duplicated

The USB endpoint must emit the same message set with the same identity as the radio
link, and must act on the same inbound messages. Writing that a second time in
`src/cli.cpp` would make a spec requirement into a standing promise: two switches to
keep aligned, and four more copies of the identity triple to get right.

The seam is placed **above the transport**, because that is the only thing the two
paths genuinely disagree about — one allocates and queues, the other packs into a
static buffer and writes.

`include/Mavlink.h` declares, and `src/mavlink.cpp` defines:

```c
void mavlinkBuildHeartbeat(mavlink_message_t *out);
void mavlinkBuildSystemTime(mavlink_message_t *out);
void mavlinkBuildBatteryStatus(mavlink_message_t *out);
void mavlinkBuildStatusText(mavlink_message_t *out, uint8_t severity, const char *text);

// Acts on msg. Returns true when a reply must be sent, having filled *reply.
bool mavlinkHandleInbound(const mavlink_message_t *msg, mavlink_message_t *reply);
```

Each builder fills a `mavlink_message_t` the **caller** provides, which is what makes
one function serve both transports without a copy:

- `Serial1` passes the heap block it already allocates, exactly as today. No extra
  copy, and no extra stack in `TaskHeartbeat` or `TaskMavlinkBatteryStatus`, which are
  128-word tasks with no room for a 291-byte local.
- USB passes its `.bss` static, then runs `mavlink_msg_to_send_buffer` and writes.

`mavlinkHandleInbound` returning the reply by value into a caller buffer avoids a sink
abstraction — no function pointers, no callbacks, no port tag travelling through a
queue. Each port sends the reply the way it sends everything else. It also matches
what the firmware actually does: every inbound message that is answered today produces
exactly one reply.

*Alternative rejected:* `TaskCli` pushing what it parses onto `serialReadQueue` so
`TaskMavlink` handles it. `TaskMavlink` replies through `serialWriteQueue`, which
drains to `Serial1` — a `TIMESYNC` arriving on USB would be answered over the radio.
Fixing that would mean tagging queue items with their origin port and routing replies
back, which forces the USB path to have a queue and undoes the reason it has none.

*Alternative rejected:* duplicating the dispatch in `src/cli.cpp`, on the grounds that
there are only two inbound cases today. The identity triple and the message semantics
are exactly what the spec requires to be identical on both ports; duplication makes
that a discipline rather than a fact, and the `default` branch — which already has a
defect logged against it — would need fixing in two places.

**Consequence for scope.** This refactors `src/mavlink.cpp`: the `send*` functions
split into a builder plus a queue push, and `TaskMavlink` becomes a thin loop over
`mavlinkHandleInbound`. That is more churn in a working file than this change would
otherwise cause, and it is the price of the guarantee.

### A second parser channel

`src/serial.cpp` parses `Serial1` with `mavlink_parse_char(MAVLINK_COMM_0, ...)` and
its own static `mavlink_message_t` / `mavlink_status_t`. The console uses
`MAVLINK_COMM_1` with a second pair, roughly 300 bytes in `.bss`. Separate status
structs are what make the sequence numbering and parse state independent rather than
interleaved.

madflight goes further and sets `MAVLINK_COMM_NUM_BUFFERS` to 0 to elide a copy. Not
copied here: it is a global build-level change to the MAVLink library affecting the
working parser on the radio link, for a saving measured in one memcpy per frame.

### Resource ownership

| Resource | Owner | Change |
|---|---|---|
| USB CDC port | `src/cli.cpp` | unchanged owner, now speaks two protocols on it |
| `Serial1` UART | `src/serial.cpp` | untouched |
| `serialWriteQueue` / `serialReadQueue` | `src/serial.cpp` drains, producers in `src/mavlink.cpp` | untouched; USB does not use them |
| SD card, clocks, ADC | as before | untouched |
| MAVLink message construction and inbound semantics | `src/mavlink.cpp`, via `include/Mavlink.h` | one definition, two callers |

The console task reads battery and time through the same library wrappers the existing
producers use, which are the designated single owners of those resources.

### Deleting `bench`

`bench` exists so the HIL suite can reach the link without a USB-TTL adapter. This
change gives every build a MAVLink endpoint on USB, which is that requirement met
permanently. Keeping the environment would leave two ways to get MAVLink onto USB and
an override, `LINK_SERIAL`, whose only remaining user is gone.

Deleting it means `platformio.ini` loses an environment, `test_filter` and
`default_envs` stop needing to disambiguate a bench build, and — the part worth
stating — **the HIL suite starts running against the firmware that flies**, which is
strictly better evidence than it had before.

## Risks / Trade-offs

- **The one-way switch collides with the HIL suite.** `run.py` opens the port and
  starts framing immediately, which would put USB in MAVLink mode and make the CLI case
  from `add-console-cli` unreachable for the rest of the run. → The CLI case must run
  before any MAVLink case on that port, or the runner must reset the board between the
  two groups. The UNO R4 resets on a 1200-baud touch of the CDC port, which gives a
  clean mechanism; the task list picks one and pins the ordering explicitly rather than
  leaving it to file-discovery order.
- **Losing `check_silence.py` loses a real check.** It is what proved the link had
  actually left USB. → Its replacement is the pair of cases asserting that USB stays in
  CLI mode until a valid frame arrives and switches after one; that is a stronger
  statement than silence, but it must actually be written, not just intended.
- **`availableForWrite()` may not report usefully on this CDC implementation.** If it
  returns 0 when disconnected the design works; if it returns a large constant
  regardless, writes can still block. → A board check before the rest is built: measure
  it connected, disconnected, and with the host not draining. madflight ships a runtime
  warning for a transmit buffer under 255 bytes, which suggests the figure varies enough
  between cores to be worth measuring rather than assuming.
- **Stack growth on a task that formats text and now also packs frames.** → The 384-word
  figure is an estimate; `ps` reports the console task's own row, and
  `configCHECK_FOR_STACK_OVERFLOW=2` is on.
- **Two endpoints with one identity confuses a bridged setup.** If someone connects both
  ports to the same GCS through a router, it sees one vehicle over two links with
  independent sequence numbers, which is what MAVLink expects — but a naive bridge that
  forwards between them would loop. → Out of scope, and stated in the spec as "not a
  router" so nobody adds forwarding later thinking it was an oversight.
- **A ground station on USB during flight is a path that did not exist before.** The
  same commands the radio link accepts are now reachable over USB. Today `TaskMavlink`
  acts on `SYSTEM_TIME` and `TIMESYNC` only, so the surface is small — but it is no
  longer true that USB cannot change firmware state, and the next command handler
  inherits that. → Noted here so it is a decision rather than a discovery.

## Open Questions

- Whether the console task should stop scheduling telemetry entirely when the USB port
  reports itself disconnected, rather than packing frames and dropping them. It is a
  CPU optimisation on the lowest-priority task, changes no observable behaviour, and
  can be settled with the `availableForWrite()` measurement.
