## Why

The housekeeping log exists for one purpose — to size the stacks (`ARCHITECTURE.md`
§5.2) — and it is the one pipeline whose output nobody can read. `src/sdwrite.cpp`
builds an ArduinoJson document per sample and `lib/SdData` serialises it as
MessagePack into `data<i>.mpk`. Nothing in this repository, and nothing on the
ground, opens one of those files: there is no script, no note and no test. The
procedure `CLAUDE.md` prescribes — "check that task's high-water mark in the SD log
after changing a task body" — has never been written down because it cannot be
carried out.

The format is also the most expensive one available. Counting the encoding of the
fifteen fields `src/sdwrite.cpp` writes today, one record is ~251 B of which **219 B
are field-name strings** rewritten 86 400 times a day — 87 % of the flight log is the
word `uartWriteAvailableStack` and its fourteen siblings. And MessagePack has no
record framing, so the expected way for a CubeSat log to end — power dying mid-write —
leaves the remainder of the file unparseable; worse, the next boot appends to the same
file, putting the corrupt stretch in the middle rather than at the end.

## What Changes

- **BREAKING** — the log format becomes **ArduPilot DataFlash** (`data<i>.BIN`
  instead of `data<i>.mpk`). Existing `.mpk` files on a card are not converted and
  stop being written to; the three incompatible record shapes a card may already hold
  (`ARCHITECTURE.md` §5.2) become a fourth, in a different format and under a
  different extension.
- **BREAKING** — `SdData::write(const JsonDocument&)` becomes
  `SdData::writeRaw(const uint8_t*, size_t)` plus `SdData::write(const uint8_t*,
  size_t)`, returning `lib/SdData` to being format-agnostic. `SdData` gains
  `setOnOpen(OnOpen)`, invoked after it opens a file so the format's owner can emit
  the preamble. This is the public API of a library `test/test_libs/test_main.cpp`
  exercises.
- The one wide record becomes four messages: `FMT` (the format definitions), `TIME`
  (wall clock and its origin), `SYS` (heap and the seven stack high-water marks, 1 Hz)
  and `PWR` (battery millivolts and percentage, at the rate the battery is actually
  read). ~34 B/s against today's ~251 B/s.
- `sdWriteQueue` **stops carrying a heap pointer and carries its item by value**, as
  `linkReadQueue` and the two write queues already do. This is forced, not chosen:
  see *Impact*.
- **`TaskMavlink` becomes a second producer** into `sdWriteQueue`, for `PWR` (from the
  schedule entry that already reads `lib/Battery`) and `TIME` (when `setUnixTime()`
  reports it accepted a time). `TaskSdWrite` builds its own `TIME` on file open, since
  rotation happens inside it.
- ArduinoJson is removed from `lib_deps`, taking with it the only recurring allocator
  in the firmware that does not follow a queue ownership protocol — a `malloc`/`free`
  pair on the **newlib** heap once a second, for ever.
- `configTOTAL_HEAP_SIZE` drops to the minimum the port accepts. Also forced: see
  *Impact*.
- Two `TODO.md` entries are absorbed rather than left behind: *[The SD log never
  stores the battery data]* and *[`uptime` overflows after ~49.7 days]*.

**This change does not alter the MAVLink surface.** No new message id, no new or
changed stream rate, no change to the identity triple (system `1`,
`MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`). `TaskMavlink` gains a queue send on a
path it already runs.

**It does change two telemetry values, which the sentence above does not cover and
which implementation surfaced rather than planning.** Once nothing calls
`pvPortMalloc`, the linker drops the FreeRTOS heap array and the two counters behind
`xPortGetFreeHeapSize()` are never initialised, so the housekeeping stream's
`HeapFree` and `HeapMin` report **0** where they previously reported 496 and 440. The
message set, its rates and its identity are untouched; what a ground station reads
in those two of nine values is not. Accepted deliberately — a firmware with no heap
reporting no heap is truthful, and the alternative of repointing them at the newlib
heap is real work with its own board verification, recorded as worth doing but not
here. See `tasks.md` task 7.2.

## Capabilities

### New Capabilities

- `flight-log`: what the flight log guarantees to whoever reads it — that a standard
  tool parses it, that one file is readable on its own, that a truncated record does
  not cost the rest of the file, that a recorded figure came from a reading rather
  than from a struct nobody filled, and that a clock change is visible in the record
  rather than appearing as an unexplained discontinuity. The log's existence and its
  bounded footprint on the card are behaviour this capability now states; no spec
  covered them before.

### Modified Capabilities

None. Two existing capabilities are *engaged* but neither has a requirement whose text
changes:

- `memory-budget` **requirement 4** ("The firmware SHALL NOT reserve RAM for a
  scheduler facility that no code in the firmware invokes") is what forces
  `configTOTAL_HEAP_SIZE` down, because after this change no code calls
  `pvPortMalloc`. The change satisfies the requirement; it does not alter it.
- `memory-budget` **requirement 5** pins that static-versus-dynamic allocation is not
  observable, and names "the housekeeping record keeps its cadence and its fields" as
  part of that. The record's fields do change here, and the honest statement is that
  the **format** is the cause and not the allocation strategy — which this change
  alters at the same time. That coincidence is the one thing a reviewer should look at
  hardest, and `design.md` argues it rather than asserting it.

`system-clock` is read from but unchanged: this change adds readers of
`sinceBootUsec()`, which that capability's spec does not constrain. The indivisibility
that adds requires is not observable by a ground station, an operator or the build, so
by this project's own spec rules it belongs in `design.md`, not in a requirement.

## Impact

**Other active changes.** There are none — `openspec list` reports an empty set, so
this change invalidates no other change's claims. It does invalidate figures in the
`TODO.md` entry it comes from, which is deleted in the proposing commit: that entry
quotes a baseline of `committed 31300 B, headroom 1468 B, .bss 21060` and a
`sizeof(Data)` of 56 B, all superseded below, and its `SYS` labels
(`Log,HB,Sta,SD,MAV,SRd,SWr`) predate both `fold-periodic-telemetry-into-mavlink-task`
and `replace-console-cli-with-usb-mavlink-link`.

**RAM.** Read from `pio run` on the flight environment at the time of writing, not
quoted:

```
  .data 740 · .noinit 28 · .bss 19744 · .heap 8192
  .stack_dummy 1024 · .vector_table 256
  committed 29984 B of 32768 (91.5 %), headroom 2784 B (minimum 1024)
  Flash 93408 B of 262144 (35.6 %)
```

This change adds no task. The queue arithmetic, which by this project's rules is what
proposing a depth means:

| | today | after |
|---|---|---|
| item | `Data*` (4 B) to a 44 B heap block | `SdRecord` by value, 32 B |
| depth | 4 | 4, unchanged |
| backing | FreeRTOS heap: 4 + 1 producer + 1 consumer = 6 × 44 = 264 B | `.bss`: 4 × 32 = 128 B |
| `configTOTAL_HEAP_SIZE` | 0x200 (512 B), backing this queue alone | minimum the port accepts |

There is no producer/consumer margin to add on top, for the reason `ARCHITECTURE.md`
§4 already gives for the link queues: with a by-value queue an item held before a send
or after a receive is a local on that task's own stack, not a shared block, so the
depth alone is what the storage needs. Depth stays at 4 even though there are now two
producers — a dropped record costs one housekeeping sample and remains **silent**,
which is *[SD logging failure is silent]*'s territory and is deliberately not fixed
here.

`SdRecord` is 32 B rather than the 24 B of its largest payload because `TimeUS` is a
`uint64_t`: the union takes 8-byte alignment, so the tag costs 8 B of the struct.

Net RAM is `.bss` +128 B against a heap reduction of up to 512 B. The direction is
favourable but the figure is **not promised** — it is a task with a build to read,
because whether `heap_4.c` accepts a size of zero is a property of the port, not of
this proposal.

`.heap` (8192 B, newlib, `BSP_CFG_HEAP_BYTES` in the variant's `bsp_cfg.h`) is a
compile-time constant and does **not** shrink by dropping ArduinoJson. What goes is
the 1 Hz churn through it. Whether 0x2000 is several times what the remaining users
need is a question nothing in this project measures, and it stays out of scope.

**Flash.** Expected to fall with ArduinoJson gone and `snprintf`-free record building,
but no figure is claimed: the baseline to measure against is the 93408 B above.

**Code.** `src/sdwrite.cpp`, `src/logger.cpp`, `src/mavlink.cpp`, `src/main.cpp`
(queue storage and item type), `include/Data.h` (becomes `include/SdRecord.h` or is
replaced), `lib/SdData/*`, `platformio.ini` (`lib_deps`, `configTOTAL_HEAP_SIZE`),
`test/test_libs/test_main.cpp` (`cleanSdFiles()` names `data*.mpk`),
`ARCHITECTURE.md` §4 and §5.2, `CLAUDE.md` (the `sdWriteQueue` invariant and the
`data*.mpk` wording), and the CI grep in `.github/workflows/main.yml`, which can widen
from two files to all of `src/` once nothing allocates.

**Verification, stated plainly.** CI can build it, run `pio check` and run the greps.
It cannot tell whether the log is readable. Reading a `.BIN` back needs the card
**pulled and read on another machine**; `TaskSdWrite`'s and `TaskLogger`'s stack
high-water marks need the **assembled board**; and the `SdData` API change drags in
`pio test -e libs`, which is **destructive** — it deletes the card's log files on every
case and must be followed by `pio run -t upload`. `tasks.md` marks each of these.

One requirement in `specs/flight-log/spec.md` is **not exercisable by anything
available**: that the log's time reference does not wrap. Demonstrating it needs an
uninterrupted run past the point where a 32-bit millisecond counter would have wrapped
(~49.7 days). It is written because the behaviour matters and because the change is
what retires the defect, but nothing in this project can close it, and after archiving
the spec will read as contract with no way to tell it apart from a proven requirement.
That is said here so it is on the record.

**`pio check`.** The baseline is 13 LOW findings. Removing a library and rewriting
`src/sdwrite.cpp`'s body will move it. The count is to be re-read and any change
explained rather than absorbed — the same debt *[Finish what add-degraded-mode left
open]* 9.5 records from the last time it drifted unexplained.
