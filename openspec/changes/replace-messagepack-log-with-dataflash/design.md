## Context

See `proposal.md` — *Why* for the motivation. What shapes the approach:

- **`src/sdwrite.cpp` owns the card** and is the only file that performs I/O on it
  (`ARCHITECTURE.md` §6). `lib/SdData` is a library it calls, not a second owner.
- **`lib/SystemTime` owns both clocks.** Its `sinceBootUsec()` header comment
  anticipates this change by name: the clock write and the epoch update in
  `setUnixTime()` are two stores, safe only while the single reader is also the single
  writer (`TaskMavlink`). This change adds two readers.
- **CI greps `src/mavlink.cpp` for `pvPortMalloc`** and fails the build
  (`.github/workflows/main.yml:58`), an invariant `queue-mavlink-messages-by-value`
  established.
- **`memory-budget` requirement 4** forbids reserving RAM for a scheduler facility no
  code invokes.
- The only `pvPortMalloc`/`vPortFree` in the firmware are `src/logger.cpp:42`,
  `src/logger.cpp:46` and `src/sdwrite.cpp:35`, all three serving `sdWriteQueue`.

## Goals / Non-Goals

**Goals:**

- Keep all format knowledge in the file that owns the card, and return `lib/SdData` to
  being a format-agnostic ring.
- Make the record set extensible without breaking old logs, so the three backlog
  features that each say they change the log schema become additive.
- Leave the log's cadence and the set of figures it carries unchanged in substance:
  this is a re-encoding, not a re-specification of what is measured.

**Non-Goals:**

- **The `flush()` policy stays as it is** — one `sync()` per record. The write
  amplification that causes (each `sync()` swaps `SdVolume`'s single 512 B cache block
  between the data sector and the directory entry, so a 34 B record costs the same
  physical traffic as a 251 B one) is real and is *not* addressed here. The resync
  framing this change introduces is what will later make relaxing it safe; doing both
  at once would mean changing the format and the durability guarantee in one step, with
  no intermediate state to attribute a regression to.
- **The ring's default size stays at 4 × 1 GiB.** It only becomes worth changing once
  the format is smaller, and the criterion for the new size lives in a separate backlog
  entry.
- **Error propagation stays absent.** `begin()` and `write()` still cannot tell
  `TaskSdWrite` that a write was thrown away. `setOnOpen` is chosen partly because it
  keeps `write()`'s signature free of a return value this change is not ready to define
  (see *Decisions*).
- Measuring the newlib heap's high-water mark, or shrinking `BSP_CFG_HEAP_BYTES`.

## Decisions

### Why ArduPilot DataFlash, over ULog and over a MAVLink `.tlog`

Three properties decide it, and they are specific to this project rather than general
merits:

- **The reader is already a dependency.** `test/test_hil/requirements.txt` pulls in
  `pymavlink`, whose `DFReader` parses these files with arbitrary format definitions and
  which ships general-purpose dump and explore tools. The reference GCS is MAVProxy,
  from the same family. Nothing new has to be installed, written or maintained on the
  ground for the log to be readable, which is the whole point of the change.
- **Every record opens with a two-byte synchronisation marker**, which is what satisfies
  *A truncated record costs only that record*. No other candidate gives this, and it is
  the property the current format most conspicuously lacks.
- **Format definitions make schema evolution additive.** A new sensor declares a new
  record type rather than widening an existing one, and old files stay readable.

`.tlog` was rejected on size: carrying the stack figures as individual named scalars
costs roughly 40 B each once a MAVLink 2 header, CRC and tlog timestamp are counted, so
nine scalars a second would be worse than the format being replaced. ULog is excluded
for needing a reader this project does not already have. A private binary format would
fix the size and fail the whole purpose.

### The queue carries meaning, not bytes — and carries it by value

`sdWriteQueue`'s item becomes a tagged union of *what a producer means*, and
`src/sdwrite.cpp` is the only place that turns one into log bytes. This mirrors
`queue-mavlink-messages-by-value` exactly (`ARCHITECTURE.md` §4): `LinkMsg` is what a
producer means and `mavlinkPack()` in the file that owns the protocol is the only thing
that forms a frame. Applied here, format knowledge stays with the card's owner, and a
producer that wants to log something never learns the encoding.

**By value is forced, not preferred.** `PWR` must come from `TaskMavlink`, because that
task already reads `lib/Battery` on its own schedule; producing it from `TaskLogger`
instead would make `Battery`'s unguarded 125 ms cache state shared between two
priorities, which is the one rule that keeps this firmware free of mutexes. But CI
forbids `src/mavlink.cpp` from calling `pvPortMalloc`. A heap-pointer producer in that
file is therefore not available, and the item must travel by value.

The consequence is that no code calls `pvPortMalloc` afterwards, which puts
`memory-budget` requirement 4 in play and forces `configTOTAL_HEAP_SIZE` down to the
minimum the port accepts. The exact minimum is a build question — whether `heap_4.c`
accepts zero is a property of the port — and is a task with a number to read, not a
figure this document asserts.

Alternative considered: keep the heap-pointer protocol and let `TaskLogger` produce all
three record kinds, reading the battery itself. Rejected for the cross-priority cache
sharing above, which is a correctness argument rather than a stylistic one.

### `writeRaw` beside `write`, and a callback on open

`lib/SdData` gains two entry points and one hook:

```
  writeRaw(bytes, n)   appends; does NOT check the size limit or rotate
  write(bytes, n)      = writeRaw, then check the limit, and on rotation:
                         close, advance index, persist index, open, invoke onOpen
  setOnOpen(cb)        cb is called after any successful open, including begin()'s
```

The split is what makes the callback safe. `onOpen` writes the preamble, and it does so
through `writeRaw`, which does not check the limit and therefore cannot rotate — so the
callback cannot re-enter the rotation path that invoked it. Were there a single `write`,
a preamble written from inside rotation could trigger rotation again.

Ordering: the preamble lands immediately after the open, before any record, which is
what *Each log file is readable on its own* requires. The size check happens on the
next `write()`, so a preamble never rotates the file it was just written into.

The callback is a plain function pointer (4 B in `.bss`), not a `std::function` and not
a capturing lambda: no allocation, and it reaches the global `systemTime` by `extern`
the way `src/logger.cpp` already does.

Alternatives considered:

- **A preamble blob handed to `SdData`** (`setPreamble(ptr, len)`). Simpler, but the
  wall-clock record belongs in the preamble and cannot be a compile-time constant, so
  `SdData` would have to report that it rotated and let `src/sdwrite.cpp` append the
  clock record afterwards — which means `write()` grows a return value, and defining
  what that value means is the error-propagation work this change excludes.
- **Moving rotation up into `src/sdwrite.cpp`**, leaving `SdData` a size-capped writer.
  Conceptually the cleanest split and it would also settle `begin()`'s idempotency, but
  it rewrites the library wholesale and widens the surface the destructive Unity suite
  covers. Deferred, not dismissed.

### The record set

Four record types. Field widths are chosen so each record is a packed structure that
`memcpy` forms — no `snprintf`, no formatting, nothing that consumes stack on a
96-word task.

| name | format | labels | bytes | when |
|---|---|---|---|---|
| `FMT` | `BBnNZ` | `Type,Length,Name,Format,Columns` | 89 | preamble, one per type |
| `TIME` | `QIB` | `TimeUS,Unix,Src` | 16 | file open, and on an accepted clock set |
| `SYS` | `QHHHHHHHH` | `TimeUS,Heap,Log,SdW,Mav,SRd,SWr,URd,UWr` | 27 | 1 Hz |
| `PWR` | `QHb` | `TimeUS,mV,Pct` | 14 | with the battery's own read cadence |

The DataFlash format's own limits are satisfied with room to spare: `name` is `char[4]`
(longest here is 4), `format` is `char[16]` (longest is 9), and `labels` is `char[64]`
(longest is `SYS`'s at 39 characters).

`SYS`'s labels are the seven tasks that exist now — logger, SD write, MAVLink, and the
UART and USB halves of the link, with `SRd`/`SWr` for the UART pair and `URd`/`UWr` for
the USB pair. The backlog entry this change comes from carried labels from two
generations earlier and they are not reused.

`Src` takes its values from `SystemTime::Source` (`Ground=0, Ds1307=1, Survived=2,
None=3`) rather than defining a second enumeration, which is what the spec requirement
about a single set of origins means in practice.

The preamble is 4 × 89 = 356 B and is **entirely constant**: format definitions never
vary at run time, so they live in flash as a `static const` array and cost no RAM. The
clock record is appended after it by the callback, because it is the one part that
carries a live value.

Splitting `PWR` from `SYS` is what satisfies *A recorded figure comes from a reading*.
Today `src/logger.cpp`'s designated initialiser omits `energy` entirely, so every record
ever written carries `millivolts: 0` and `remaining: 0`; with a separate record emitted
by the task that actually reads the battery, an absent battery produces no record rather
than a zero.

### Resource ownership after the change

| resource | owner | how anyone else reaches it |
|---|---|---|
| the SD card | `src/sdwrite.cpp` | `sdWriteQueue` |
| the log's byte format | `src/sdwrite.cpp` | nothing else forms log bytes |
| the ring, index and rotation | `lib/SdData`, called only by `src/sdwrite.cpp` | — |
| both clocks | `lib/SystemTime` | its accessors |
| the ADC / battery | `lib/Battery`, read by `TaskMavlink` only | `PWR` records |
| both link ports | `src/link.cpp` | unchanged by this change |

`TaskMavlink` gaining a `sdWriteQueue` send does not make it a second owner of the card:
it posts a value to a queue and never touches the medium.

### Making the clock pair indivisible

`setUnixTime()` writes the clock and updates the boot epoch as two stores. With
`TaskLogger` and `TaskSdWrite` added as readers of `sinceBootUsec()`, a reader landing
between them gets a wrong elapsed time. The fix the header prescribes is to **suspend
the scheduler across the pair**, not to add a mutex — consistent with a firmware whose
freedom from mutexes rests on single ownership, and cheap because the pair is two
stores.

This is not a spec requirement: a reader that lands in the window gets one wrong time
figure in one record, which no ground station, operator or build can distinguish from a
correct one. It is a correctness obligation of the design, which is why it is stated
here.

Adopting `sinceBootUsec()` as the time reference is also what retires the `uptime`
field's 49.7-day wrap: elapsed time becomes a subtraction against a boot epoch, with no
accumulator to maintain and nothing to wrap. The resolution becomes the RTC's 1/128 s
rather than the tick's 1 ms, which is immaterial for records at 1 Hz and below.

## Risks / Trade-offs

**Two producers now post to `sdWriteQueue`, and a dropped record is still silent.** →
Not mitigated, deliberately. Depth stays at 4 and a record the queue refuses is lost
without trace, as today. Naming it here so that the first person to suspect a gap in a
log knows it is a known hole and not a new one; the fix is a separate backlog entry.

**The allocation strategy and the record format change in the same step**, and
`memory-budget` requirement 5 says static-versus-dynamic allocation must not be
observable. → The record set does change observably, and the cause is the format, not
the allocation. The way to keep that honest is to verify the two independently: the
by-value queue is verifiable by reading the same figures off the link's housekeeping
stream, which this change does not touch, while the record change is verified by reading
a file back. `tasks.md` keeps them as separate steps for that reason.

**`configTOTAL_HEAP_SIZE` may not accept the minimum the requirement implies.** →
Whether `heap_4.c` builds and runs with a size of zero is unverified. If it does not,
the smallest size that does is what ships, with the figure and the reason recorded; the
requirement is about not reserving for an uninvoked facility, and a port-mandated
floor is not a reservation this firmware chose.

**Changing `SdData`'s public API drags in the destructive Unity suite.** → `pio test -e
libs` deletes the card's log files on every case and replaces the firmware with the
Unity binary. Permission is asked before running it and `pio run -t upload` follows, per
`CLAUDE.md`.

**Three record shapes already exist on cards in `.mpk` files, and this adds a fourth
format under a new extension.** → No conversion is offered and none is planned. Old
`.mpk` files remain on the card until the ring's file names — which change extension —
stop colliding with them, meaning they are simply never reclaimed. An operator who wants
the space back deletes them by hand. This is recorded rather than solved because
reclaiming them would mean the firmware deleting files it did not write.

**A wrong `FMT` definition is silent.** → A mismatch between a declared format string
and the structure actually written produces plausible-looking wrong numbers rather than
a parse error. The mitigation is a compile-time assertion that each record structure's
size equals the length its definition declares, which costs nothing and catches the
whole class.

## Migration Plan

No staged rollout: the board runs one firmware. Rollback is reflashing the previous
image, after which the firmware resumes writing `.mpk` files and the `.BIN` files it
wrote are left on the card, readable. The two formats never share a file because they
never share a name.

## Open Questions

- Whether `configTOTAL_HEAP_SIZE` can be zero or must keep a port-mandated floor. Safely
  deferred: it changes one number in `platformio.ini` and no spec, approach or task
  boundary, and the build answers it.
- Whether `include/Data.h` is edited in place or replaced by an `include/SdRecord.h`.
  Cosmetic; it changes no interface beyond the two files that include it.
