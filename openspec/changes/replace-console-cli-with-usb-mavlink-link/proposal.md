## Why

The USB CDC port carries a read-only text CLI and the hardware UART carries the
MAVLink link, and no build has both at once: reaching the link over USB means
rebuilding with `-D LINK_SERIAL=Serial` (the `bench` environment), which takes
the CLI away, and reaching the CLI means the link is only on D0/D1 where it
needs a USB-TTL adapter. That split costs a reflash every time the bench needs
the other one, and it has a sharp edge already recorded in the backlog: a board
that has latched into the reduced configuration is recovered by
`MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, which needs an active MAVLink connection,
which the flight build offers only on D0/D1 — so the board has to be reflashed
to be rescued.

Making USB a second, always-on MAVLink endpoint removes the reflash from both
paths: the HIL suite runs against the flight build over the USB cable alone, and
a stuck board is commandable without an adapter. The CLI is what the port gives
up to make room.

## What Changes

- **BREAKING**: the USB CDC port stops being a text console and becomes a second
  MAVLink endpoint, active in every build. A host opening it at 115200 now sees
  MAVLink frames, not a prompt.
- **BREAKING**: the `ps`, `ps <name>`, `free`, `help` and `?` commands are
  removed, with `src/cli.cpp` and `include/Cli.h`. The numbers they reported
  remain reachable as the `NAMED_VALUE_INT` housekeeping stream on either port,
  but only to a ground station, not to a plain terminal — see *Impact*.
- **BREAKING**: the `bench` environment and the `CLI_SERIAL` override are
  retired. They existed only to move MAVLink onto USB for bench work, which is
  now the default state, so `.github/workflows/main.yml` drops `-e bench`.
- Each port is an independent MAVLink stream: its own sequence numbering, its
  own housekeeping arming, and replies leaving by the port their request
  arrived on. Both keep the unchanged identity triple, so a ground station sees
  one vehicle on whichever port it is attached to.
- `src/serial.cpp` becomes `src/link.cpp` and owns both ports through a new
  per-port descriptor in `include/LinkPort.h`. `src/mavlink.cpp` remains the only
  file that knows what a `msgid` is, now channel-aware.
- The task set gains `UsbRead` and `UsbWrite`, renames `SerialRead`/`SerialWrite`
  to `UartRead`/`UartWrite`, and loses `Cli` — seven tasks where there are six.

**This alters the MAVLink surface.** No new message id and no new stream rate,
and the identity triple is untouched. What changes is that every message this
firmware already emits or accepts is now emitted and accepted on a second port,
with independent per-port sequence numbering. Sequence numbering is the part
that is not free: `src/mavlink.cpp`'s seven `mavlink_msg_*_pack()` calls
implicitly use channel 0's counter, so on two ports both streams would share one
sequence and each ground station would see gaps and infer packet loss. They
become `mavlink_msg_*_pack_chan()`.

## Capabilities

### New Capabilities

None. Both ports serve the behaviour `mavlink-link` already defines.

### Modified Capabilities

- `mavlink-link`: the requirement that the firmware "SHALL NOT exchange MAVLink
  frames over the USB CDC port" is reversed. The capability gains per-port
  requirements — independent sequence numbering, replies routed back to the
  originating port, per-port housekeeping arming — and its "No task waits for the
  link port to become ready" requirement has to be qualified, because a CDC port
  has a readiness state that a UART does not.
- `console-cli`: **removed in full.** Every requirement it states describes a
  console this change deletes, so the delta removes the capability rather than
  modifying it, and `openspec/specs/console-cli/` goes with it on archive.

## Impact

**RAM, against a build run for this proposal** (`pio run -e uno_r4_minima`,
committed 27336 B of 32768, **headroom 5432 B**, floor 1024):

```
                                        predicted    measured (built)
  + UsbWrite  stack 384 w + TCB           +1636
  + UsbRead   stack  96 w + TCB            +484
  + LinkPort x2 (rx item + parser state)   ~+345      672 B total, less
                                                      the 327 B of file
                                                      statics it replaces
  + usbWriteQueue 5 x sizeof(LinkMsg)       +320      +320 (320 B)
  + linkReadQueue item gains the chan tag  ~+104        +8 (2336 vs 2328;
                                                      sizeof(InboundMsg)
                                                      is 292, not ~304)
  + MAVLINK_COMM_NUM_BUFFERS 1 -> 2         +363      +303 (m_mavlink_buffer
                                                      291->582, and the two
                                                      m_mavlink_status copies
                                                      36->48 each)
  - TaskCli deleted (128 w stack + TCB)     -612
                                          -------    ----------------
                                           ~2640      +2132 B actual
```

**Measured, not projected.** `pio run -e uno_r4_minima` before this change
reported committed 27336 B / headroom 5432 B; after it, **committed 29852 B of
32768 (91.1 %), headroom 2916 B** against the 1024 B floor. The real cost is
**+2516 B**, of which 384 B is two stacks grown after measuring them on the
board (`UsbRead` 96 -> 128 w, `TaskLogger` 96 -> 160 w; see tasks 9.6 and 9.7).
The structural part came to +2132 B, about 500 B under the estimate: the inbound item's alignment
padding came to 8 B rather than the ~104 B assumed, and the second MAVLink
channel to 303 B rather than 363 B.

`platformio.ini:76-84` already anticipated this change and told it to carry
"~315 bytes" for the channel. That estimate counted one `m_mavlink_status` copy
where the linker emits two, and assumed both would scale linearly; the measured
303 B is close to it by coincidence rather than by the same arithmetic. The
comment and the `-D MAVLINK_COMM_NUM_BUFFERS` value are both updated in this
change.

**The FreeRTOS heap is re-derived, not merely checked**, and doing so found a
pre-existing error. `include/Data.h` goes from five per-task stack fields to
seven, taking `sizeof(Data)` from **36 B to 44 B** — measured with a
compile-time probe, not counted by hand. `ARCHITECTURE.md` §4 had claimed 56 B
and a 336 B backing; the real figure had been 6 x 36 = 216 B and was never
right. It is now 6 x 44 = **264 B** against `configTOTAL_HEAP_SIZE` 0x200 =
512 B, leaving 248 B. On the board the heap's minimum-ever-free read 448 of
512, so the observed peak is 64 B — the queue rarely holds more than one item.
§4's table is corrected to the measured values.

**The SD log schema changes**, which makes a third record shape on a card that
may already hold two. `ARCHITECTURE.md` §5.2 distinguishes the oldest one as
"the old seven-field shape"; the new shape also has seven fields, so after this
the count no longer tells them apart and that sentence misleads whoever parses
the `.mpk` files. The record grows from ~254 B to ~295 B, so ~25.5 MB/day
instead of ~22.

**Code.** `src/cli.cpp` and `include/Cli.h` deleted; `src/serial.cpp` renamed to
`src/link.cpp`; `include/LinkPort.h` added; `src/mavlink.cpp`, `src/main.cpp`,
`include/Link.h`, `include/Data.h`, `src/logger.cpp`, `src/sdwrite.cpp`,
`platformio.ini` and `.github/workflows/main.yml` modified. `ARCHITECTURE.md`
sections 2, 3, 4, 5.1, 6 and 8 all carry statements this falsifies — the system
diagram, the `pvParameters` rule, the queue table, the single-parser claim, the
console row in the ownership table, and the four CLI commands — and are updated
in the same commit, as is `CLAUDE.md`'s console paragraph.

**Tests.** `test/test_hil/check_cli.py` is deleted with the capability it covers
and `check_silence.py` asserts the opposite of what this makes true, so it is
inverted. The suite's default target becomes USB, which means it runs against the
flight build with no adapter attached — the thing `bench` existed to provide.
Five new cases cover the per-port claims, one of them a manual `[board]` step.

**No other change is in flight.** `openspec/changes/` holds only `archive/`, so
there is nothing whose claims this could contradict. Two `TODO.md` entries are
affected instead and neither is absorbed here: *Debug and release builds, with
MAVLink tracing on the console* plans to surface a trace "through the CLI", which
this deletes, and *Record which scenario each HIL case covers* is the mapping
this change roughly doubles the surface of. The second is recommended before or
alongside this work, but stays its own entry — widening this change to swallow it
is not proposed.
