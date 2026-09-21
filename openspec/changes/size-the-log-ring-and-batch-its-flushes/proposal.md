## Why

Two defects sit in `lib/SdData`, a few lines apart, and they are one change because they
are the same trade seen from two sides: what a power cut costs against what avoiding it
costs.

**The ring never rotates.** `SdData`'s defaults are four files of 1 GiB. At the ~34 B/s
the DataFlash log writes, filling one takes about a year, so `writeLogIndex()`,
`index.bin` and the whole resume-after-power-cycle mechanism have **never executed on a
board doing its actual job** — the only place they run is a Unity case that uses
1024-byte files. The requirement they exist to satisfy, that the log occupy a bounded
amount of the card, is therefore held up by code nothing has exercised where it matters.
It also makes both download paths impractical: nothing the size of a gigabyte comes down
a telemetry link.

**Every record is flushed.** `writeRaw()` calls `_dataFile.flush()` after each record.
Traced through `SdFile::sync()` and `SdVolume::cacheFlush()`, one 27-byte `SYS` record
costs **two sector writes and two sector reads**: the single 512 B cache block is evicted
by the `sync()` that just wrote it, so the next record reads it back. The flush is paid
twice. That is 3.0 sector writes a second, and **half of them land on the same directory
sector** — about 129 600 times a day, 47 million times a year on one logical sector. What
wears out is not the log, it is the filesystem that indexes it, and with it the other
three files.

Neither is urgent on its own. Together they are, because the second cannot be verified
without the first: a buffering scheme's interaction with rotation is only observable on a
board that rotates, and today none does. Shipping the flush change alone would mean
accepting exactly the kind of unexercised path that the ring is already an example of.

## What Changes

- **The ring's defaults become 4 files of 1 MiB.** Rotation then happens about every
  8.6 hours, `index.bin` stops being dead code, and each file comes down a 57 600 baud
  link in under four minutes. The size is derived from download time, not picked round —
  the arithmetic is in `design.md`.
- **`write()` batches its flushes.** It accumulates bytes written and calls
  `flush()` once every 4 KiB rather than once per record, letting `SdFile::write` take
  the branch that fills a cache block and writes it once when full. About 36× fewer
  sector writes.
- **`writeRaw()` keeps flushing on every call.** It is how the `FMT` preamble reaches the
  card, and a preamble that does not land leaves the file unreadable rather than merely
  short. The asymmetry is deliberate and is the reason the two methods stay separate.
- **BREAKING, at the level of the spec**: the guarantee about what a power cut costs
  changes from one record to one flush interval. That is the price of the above and it is
  stated as a requirement rather than absorbed quietly.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `flight-log`: **modifies** *A truncated record costs only that record*. The
  requirement currently promises that "the incomplete tail is the only data lost", which
  a batched flush breaks literally — completed records in the unflushed block are lost
  too. The bound becomes the flush interval, about two minutes of log at the current
  rate, and the resynchronisation property that makes a damaged stretch recoverable is
  unchanged and still carries the rest of the requirement.

- `flight-log`: **adds** a requirement that maintaining the log never resets the board.
  This change is what makes rotation actually happen, and rotation is the one operation
  in the log path whose duration is unbounded by anything the firmware controls:
  `SD.remove()` walks a FAT chain proportional to the file size, against a watchdog
  refreshed only from the idle hook. The behaviour matters and is observable from the
  ground — a watchdog reset changes the reason the heartbeat reports and restarts
  `time_boot_ms` — so it belongs in the spec rather than only in `design.md`.

## Impact

**Code.** `lib/SdData/SdData.h` and `lib/SdData/SdData.cpp`: the two constructor defaults
and the flush accounting in `write()`. `src/sdwrite.cpp` constructs `SdData sdData;` with
no arguments and therefore inherits the new defaults without an edit — which is worth
stating, because it means the change of on-card footprint happens without any line in
`src/` moving.

**RAM.** No task, no queue, no library, and **no buffer**: the 512 B `SdVolume` cache
block already is the buffer, and today the per-record flush throws it away. The only new
state is one `size_t` counter in `SdData`. Headroom read from a build while writing this,
not quoted: **3184 B** of 32768, floor 1024.

**On-card footprint drops from 4 GiB to 4 MiB**, which is the point but is also a change
to what the card must have free. It cannot fail on a smaller card the way the old default
could.

**MAVLink surface.** Unchanged. No new message id, no new rate, same identity triple.

**Other active changes.** None under `openspec/changes/`. There is an open pull request,
*Report the card's FAT geometry from the Unity suite*, which adds a report-only Unity case
naming the cluster size and the per-file FAT cost. This change's task list depends on that
case having landed, because the cluster size is what decides whether a rotation fits inside
`WDT_TIMEOUT_MS`. It invalidates nothing this change claims; it supplies a number this
change needs.

**Verification needs the assembled board, and one step needs the power cut.** The flush
bound can only be demonstrated by removing power and counting what is missing, which no
script observes. Rotation's duration needs measuring rather than deriving: the arithmetic
in `design.md` bounds the FAT work, not the card's own write latency, which varies by an
order of magnitude between cards. `pio test -e libs` is destructive and erases the log.

**This change does not reduce card wear to its floor, deliberately.** The remaining cost
is the directory entry, and removing it entirely needs pre-allocation, which this library
does not expose — see *Replace `arduino-libraries/SD` with `greiman/SdFat`* in the backlog.
36× is what is available without a new dependency.
