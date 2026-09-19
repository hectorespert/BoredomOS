## Context

See `proposal.md` — Why. What shapes the approach here is four existing
constraints, not the motivation:

- **RAM is the binding constraint.** The build reports 5432 B of headroom above a
  1024 B floor and refuses anything that does not fit. A literal mirror of today's
  queues and tasks costs roughly 2.2 KB more than the topology below and buys no
  observable behaviour.
- **Stacks are tuned tight** (96–384 words) and a `mavlink_message_t` is 291 B, so
  where a message buffer lives is an architectural decision, not a detail.
- **One owner per resource** is what makes the absence of mutexes safe.
- **`src/serial.cpp`'s parser state is file-static**, justified in
  `ARCHITECTURE.md` §5.1 by "exactly one task parses". This change makes two tasks
  parse, so that justification expires and the state has to move somewhere.

## Goals / Non-Goals

**Goals:**

- Two MAVLink endpoints whose externally observable behaviour is symmetric, as
  `specs/mavlink-link/spec.md` requires, without duplicating protocol knowledge.
- Keep `src/mavlink.cpp` the only file that knows what a `msgid` is.
- Fit in the reported headroom with margin left for the backlog competing for it
  (the IMU, DataFlash logging, FTP, the parameter protocol).

**Non-Goals:**

- **Internal symmetry.** The two ports differ in ways the hardware imposes, and
  the design follows the hardware rather than a diagram.
- **Moving the transmit buffers off the writer stacks.** Worth doing, RAM-neutral,
  and deliberately deferred — see Decision 7.
- **Absorbing *Record which scenario each HIL case covers*.** Recommended before
  or alongside this work; it stays its own backlog entry.
- **Any change to what a port emits or accepts.** No new message id, no new rate,
  no identity change.

## Decisions

### 1. One inbound queue, tagged and shared; one outbound queue per port

```
  Serial1 --> UartRead(&port0) --+                       chan 0
              96 w, HIGHEST      |                          |
                                 +--> [ linkReadQueue ] ----+
                                 |    8 x { chan, mavlink_message_t }
  USB     --> UsbRead(&port1) ---+              |
              128 w, HIGH                       v
                                        +---------------+
                                        |    Mavlink    | 384 w, HIGH
                                        | one instance, |
                                        | both channels |
                                        +---+-------+---+
                                  chan 0    |       |    chan 1
                        [uartWriteQueue]<---+       +--->[usbWriteQueue]
                            5 x LinkMsg                     5 x LinkMsg
                                  |                             |
                                  v                             v
                          UartWrite(&port0)             UsbWrite(&port1)
                             384 w, HIGH                   384 w, HIGH
```

Two 8-deep `mavlink_message_t` queues cost 2328 B each; one tagged queue costs
2328 B plus the tag's alignment padding. Outbound stays split because the two
writer tasks must drain independently (Decision 2).

*Alternative considered — a full mirror (`usbReadQueue` + a second `Mavlink`
instance).* Rejected: ~2.2 KB more for behaviour the spec does not distinguish.
*Trade-off accepted:* a flooding port can displace the other's inbound items. This
is the existing drop-on-full behaviour, not a new failure mode, and inbound traffic
is a few frames per second.

Depths are unchanged from today: 8 inbound, 5 outbound per port. The 5 is not
round — `src/main.cpp:119` derives it from the housekeeping schedule entry
coinciding with the other three on one pass, and that reasoning applies per port
unchanged. The shared inbound depth of 8 is a reduction in worst-case per-port
depth; accepted for the same reason as the trade-off above.

### 2. Two readers, two writers, one protocol task — and the priorities follow the hardware

| Task | Stack | Priority | Why |
|---|---|---|---|
| `UartRead` | 96 w | `HIGHEST` | No flow control on D0/D1: undrained bytes are **lost** |
| `UsbRead` | 128 w | `HIGH` | CDC NAKs when full, so it can only be **delayed**; 96 measured too thin (see tasks 9.6) |
| `UartWrite` | 384 w | `HIGH` | Busy-waits, bounded by baud (`ARCHITECTURE.md` §3) |
| `UsbWrite` | 384 w | `HIGH` | Safe at `HIGH` only with Decision 6's guard |
| `Mavlink` | 384 w | `HIGH` | One instance, both channels |

This is the reason two readers exist rather than one polling both ports: a single
reader cannot hold two priorities, and the two ports have genuinely different
consequences for being late. A single reader would also let a host flooding USB
starve the UART inside one `while (available())`, losing bytes that are
unrecoverable — the USB port's bytes never are.

One `Mavlink` task because its cadences are 1 Hz; a second instance costs 1536 B
of stack to decouple schedules that do not need it. Its schedule table gains a
port column and its dispatch reads `chan` off the queued item.

### 3. The port descriptor carries function pointers, not a `Stream&`

`include/Link.h` already warns that `UART` overloads `write(uint8_t*, size_t)` as
non-const and never overrides `Print`'s virtual const version, so reaching a port
through a `HardwareSerial&` silently selects the per-byte fallback. `Serial` and
`Serial1` are different types in any case. The descriptor therefore holds pointers
onto thin per-port wrappers that name the concrete port:

```c
struct InboundMsg { uint8_t chan; mavlink_message_t msg; };

struct LinkPort {
    int    (*available)();
    int    (*read)();
    size_t (*write)(const uint8_t *, size_t);
    int    (*availableForWrite)();   // INT_MAX for the UART -- Decision 6
    QueueHandle_t     writeQueue;
    InboundMsg        rx;            // Decision 7
    mavlink_status_t  rxstatus;
};
```

*Alternatives considered:* a template on the port type (zero-overhead but task
bodies must be `void(void*)`, so it needs explicit instantiation and reads worse);
duplicated task bodies per port (no descriptor, but doubles the flash and the
places a protocol fix has to land).

### 4. `pvParameters` starts being used

Two instances of one task body need per-instance identity, so `xTaskCreateStatic`
passes `&linkPorts[n]`. This contradicts `ARCHITECTURE.md` §3 — "nothing is passed
through `pvParameters`, which is `(void)`-cast away in every task" — which is
amended in the same commit. It is what the parameter is for, and the alternative
is Decision 3's rejected duplication.

### 5. Packing becomes per-channel

`src/mavlink.cpp`'s seven `mavlink_msg_*_pack()` calls implicitly use channel 0's
`current_tx_seq`. On two ports that makes both streams share one counter, so each
ground station sees sequence gaps and infers packet loss — the spec's *Each port is
an independent MAVLink stream* would fail. They become
`mavlink_msg_*_pack_chan()`, and `mavlinkPack()` takes a `chan`. `current_tx_seq`
lives in `mavlink_status_t[chan]`, so independent numbering then costs nothing
beyond Decision 9.

### 6. The USB writer guards on `availableForWrite()`, and that is why it can sit at `HIGH`

`_SerialUSB::write()` (`cores/arduino/usb/SerialUSB.cpp:85`) differs from
`UART::write()` in two ways that matter:

- With no host attached it returns 0 immediately. Frames are discarded, which is
  correct and costs nothing.
- With a host attached but not draining, it loops `continue` while
  `tud_cdc_write_available()` is 0, **never yielding**. A task doing that above
  idle priority prevents `vApplicationIdleHook()` from running, which is where the
  watchdog is refreshed — so a host that opens the port and stops reading resets
  the board within `WDT_TIMEOUT_MS`.

Today's `src/cli.cpp` escapes this only because `PRIORITY_LOWEST` is
`tskIDLE_PRIORITY` and time slicing still gives idle its turn.

The fix is not priority. The writer checks `availableForWrite()` and drops the
frame when there is no room, which is what it already does when the outbound queue
is full. The guard lives behind a descriptor pointer returning `INT_MAX` for the
UART, so one task body serves both ports with no `if (chan == 1)` inside it.

*Alternative considered — `UsbWrite` at `PRIORITY_LOWEST`.* It would be safe, but
by accident rather than by construction, and it would put a link writer below the
SD writer.

### 7. The inbound item lives in the descriptor; the transmit buffers stay on the stack

`UartRead` runs on 96 w = 384 B with about 204 B already in use, which works only
because the parsed message is file-static today. Composing `{chan, msg}` as a local
before `xQueueSend` would copy 291 B onto that stack and overflow it. So the whole
item is parsed into `port.rx` in place and sent from there — this is forced, not a
preference.

The writers' `mavlink_message_t` and `uint8_t buf[MAVLINK_MAX_PACKET_LEN]` (571 B
together) could move the same way and let each writer drop to roughly 240 w. That
is **deliberately deferred**: it is RAM-neutral, so it buys no headroom, and
keeping the writers at the proven 384 w means the board measurement confirms a
known-good size rather than deriving four new ones at once. Worth its own entry
once the four high-water marks are known, on the argument that a struct field
cannot overflow and a tight stack can.

### 8. The reader drain is bounded at 128 bytes per port per pass

`while (available())` is unbounded. With two readers the cross-port starvation of
Decision 2 is gone, but `UsbRead` at `HIGH` can still displace `Logger` and
`SdWrite`. One constant, not per port: the UART can deliver at most 58 B into a
10 ms poll so the cap never bites there, and 128 B per pass is 12.8 kB/s sustained
against an inbound stream of a few frames per second.

### 9. `MAVLINK_COMM_NUM_BUFFERS` goes from 1 to 2

`platformio.ini:76-84` already anticipated this change and instructed it to raise
the value and carry the cost. Measured with `nm` on both images:

```
  one channel   m_mavlink_buffer 291  +  m_mavlink_status 24 x2  =  339 B
  two channels  m_mavlink_buffer 582  +  m_mavlink_status 48 x2  =  678 B
  delta                                                          = +339 B
```

**+339 B.** Three wrong figures preceded this one and all three are corrected
here: the comment's own ~315 B estimate, an earlier draft of this design that
said 363 B (which is the *total before*, not the delta), and `proposal.md`'s
+303 B. Copilot's review caught that they could not all be true; the number
above is the one the linker reports.

### 10. Task naming

`UartRead`, `UartWrite`, `UsbRead`, `UsbWrite` — the existing
`SerialRead`/`SerialWrite` are renamed. In Arduino, `Serial` *is* the USB CDC port
and `Serial1` is the UART, so keeping `Serial*` for the UART task once both are
MAVLink links points the reader at the wrong port. The binding length limit is
`NAMED_VALUE_INT.name`, which is `char[10]` and is what truncates `SerialWrite` to
`SerialWrit` on the wire today — not `configMAX_TASK_NAME_LEN`, which is 16. All
four names fit in 10 and none collide after truncation, which matters because the
housekeeping stream exists to tell per-task high-water marks apart.

A name lives in three places that must agree: `src/main.cpp`'s
`xTaskCreateStatic` string, `src/mavlink.cpp`'s `kName*` table, and
`include/Data.h`'s field names.

### 11. Ownership

| Resource | Owner after this change |
|---|---|
| `Serial1` (UART) and the USB CDC port | `src/link.cpp` — both, one file |
| MAVLink protocol, packing and dispatch | `src/mavlink.cpp` |
| SD card | `src/sdwrite.cpp` via `lib/SdData` (unchanged) |
| Clocks, ADC | `lib/SystemTime`, `lib/Battery` (unchanged) |

`src/serial.cpp` is renamed to `src/link.cpp` and becomes the owner of **both**
ports rather than one. Splitting them across two files would create two owners of
the same class of resource and weaken the argument that no mutexes are needed. The
`.github/workflows/main.yml` grep that forbids `pvPortMalloc` in `src/serial.cpp`
and `src/mavlink.cpp` must follow the rename, or it silently stops checking
anything.

## Risks / Trade-offs

- **A host that opens USB and stops reading resets the board** → Decision 6's
  `availableForWrite()` guard, and a spec scenario plus a manual `[board]` HIL step
  that exercise it. This is the single most dangerous thing the change introduces.
- **Sequence numbering silently shared if `_pack_chan` is missed** → each ground
  station infers packet loss, with nothing failing loudly. Covered by an automated
  HIL case reading the `seq` field on both ports.
- **A 291 B stack copy overflows `UartRead`** → Decision 7 makes it structurally
  impossible rather than relying on measurement; `configCHECK_FOR_STACK_OVERFLOW=2`
  remains the backstop.
- **One port floods and displaces the other's inbound items** → accepted; existing
  drop-on-full behaviour, bounded further by Decision 8.
- **The `pvPortMalloc` CI grep loses its target on the rename** → included as a
  task, not left to the rename.
- **Losing the CLI is a real diagnostic regression**, not a wash — stated in the
  proposal's *What Changes* and in the `console-cli` delta's migration notes rather
  than papered over.
- **A third `.mpk` record shape, and the second with seven fields** →
  `ARCHITECTURE.md` §5.2's "the old seven-field shape" stops being a unique
  description; reworded in the same commit.

## Migration Plan

There is nothing to migrate at runtime — the firmware holds no persisted state
this touches, and housekeeping arming is already session-scoped. What needs
sequencing is the bench:

1. Land the code, `platformio.ini` (including `MAVLINK_COMM_NUM_BUFFERS=2` and the
   removal of `bench`), the CI workflow and the docs in one commit, since
   `ARCHITECTURE.md` and `platformio.ini` both carry claims that become false the
   moment the code changes.
2. Flash and verify over USB, which needs no adapter — this is the first time the
   flight build is reachable that way.
3. Verify the UART port with an adapter on D0/D1, which is the configuration CI
   cannot reach and the one most likely to be skipped.
4. Read all four new high-water marks before calling the stacks settled.

**Rollback** is `git revert` plus `pio run -t upload`; the board needs no
intervention beyond a reflash. A board left unreachable by a bad flash still has
the double-tap RESET DFU path, which is what recovered it before.

## Open Questions

- ~~**The four stack sizes.**~~ **Answered on the board** (tasks 9.6/9.7), so
  this is no longer open. `UartWrite`, `UsbWrite` and `Mavlink` held at 384 w.
  `UsbRead` needed **128** rather than the 96 carried forward, because
  `_SerialUSB::available()`/`read()` reach TinyUSB through deeper call frames
  than the UART's. `TaskLogger` — not one of the four, and not predicted —
  needed **160**, because this change grew `sizeof(Data)` and it builds one on
  its stack.
- **Whether `UsbWrite` wants `HIGH` or something lower once measured.** Decision 6
  makes `HIGH` safe; whether the secondary link deserves parity with the primary
  under sustained load on both ports is an observation to make on the bench, not a
  precondition.
