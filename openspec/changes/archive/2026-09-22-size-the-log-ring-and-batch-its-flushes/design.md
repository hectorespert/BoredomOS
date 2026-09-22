## Context

See `proposal.md` — Why. What shapes the approach is what the bundled SD library actually
does, which was read rather than assumed.

**`SdVolume` has exactly one 512 B cache block, and it is `static`** — shared by every
file and every volume object. `SdFile::write()` has two branches. Appending at a block
boundary takes the cheap one: it does **not** read the block, it marks it dirty and fills
it, and the block is written once, when the write moves past it. Appending mid-block takes
`cacheRawBlock()`, which reads first.

**`SdFile::sync()` costs more than it looks.** Every append sets `F_FILE_DIR_DIRTY`
because the file grew, so `sync()` fetches the directory entry through
`cacheDirEntry()` → `cacheRawBlock()`, which **flushes the data block out of the cache**,
reads the directory block in, writes `fileSize`, and flushes that. The data block is then
gone from the cache, so the next record reads it back. Per 27-byte record: two sector
writes, two reads, and the flush paid twice.

**`SD.remove()` is proportional to file size.** `SdVolume::freeChain()` walks the cluster
chain one cluster at a time — `fatGet()` then `fatPut()` each — and `fatPut()` dirties the
mirror FAT as well. So a delete costs about one sector read and `fatCount()` sector writes
per FAT sector the chain *visits*, which is one per run of consecutive clusters rather than
one per cluster: contiguity is what keeps the count down, and nothing guarantees it.

**One owner, unchanged.** `lib/SdData` owns `_dataFile`, the ring, the index and rotation.
`src/sdwrite.cpp` owns the card and the meaning of the bytes and is the only caller.
`lib/SystemTime` owns the clocks, `lib/Battery` the ADC. This change adds no second owner
of anything and touches no peripheral from a new place: every edit is inside the object
that already holds the handle.

## Goals / Non-Goals

**Goals:**

- Rotation happens on a timescale a mission reaches, so the code that bounds the card
  footprint is exercised where it matters.
- The number of sector writes drops by an order of magnitude, with the loss on a power cut
  bounded, declared, and independent of uptime.
- No new RAM, no new dependency, no format change, and no edit in `src/`.

**Non-Goals:**

- Reducing wear to its floor. The directory-entry write survives; removing it needs
  pre-allocation, which this library does not expose. See *Replace
  `arduino-libraries/SD` with `greiman/SdFat`* in the backlog.
- A ground-settable file size. That needs *Implement the MAVLink parameter protocol*,
  which does not exist; both figures stay compile-time.
- Error propagation. `write()` still returns `void` and a failed open is still silent —
  that is *SD logging failure is silent*.
- Making rotation atomic against a power cut. A cut between `writeLogIndex()` and the new
  file opening still leaves the index pointing at a file that has not been created; the
  next boot opens it, which is correct by accident rather than by design, and is out of
  scope here.

## Decisions

### The ring becomes 4 × 1 MiB, derived from download time

Both download paths move about the same order of bytes: ~4.6 KB/s at `LINK_BAUD` for the
log protocol — 90 useful bytes in a 111-byte `LOG_DATA` frame at 57 600 — and ~239 useful
bytes per FTP packet. So the file size follows from how long a download may take:

| file size | covers, at 34 B/s | download |
|---|---|---|
| 1 GiB (today) | ~365 days | ~67 hours |
| 4 MiB | ~34 h | ~15 min |
| **1 MiB** | **~8.6 h** | **~3.7 min** |
| 256 KiB | ~2.1 h | ~56 s |

Four of 1 MiB give ~34 h of continuous log, rotate several times a day, and each comes
down in under four minutes. The footprint falls from 4 GiB to 4 MiB, which also removes a
failure the old default could produce: a card without 4 GiB free would let the first file
grow until a write failed, silently.

**Alternative considered: 8 × 512 KiB.** Same footprint, same total coverage, half the
download time, rotating twice as often. Rejected only because four files is what the
persisted index and every existing test assume, and doubling the count is a second
variable in a change that already has one. It remains the obvious next adjustment if
download time turns out to matter more than it appears to.

### `write()` flushes every 4 KiB; `writeRaw()` flushes every call

`SdData` keeps a byte counter since the last flush. `write()` appends, adds the length,
and flushes when the counter reaches 4 KiB. `writeRaw()` flushes unconditionally, as
today.

Per 512 B of log the cost becomes one data-block write from the cache advancing (which
would happen anyway) plus, once per interval, the partial block and the directory entry:

| flush interval | sector writes/s | factor | lost on a power cut |
|---|---|---|---|
| per record (today) | 3.00 | 1× | one record |
| 512 B | 0.199 | 15× | ~15 s |
| **4 KiB** | **0.083** | **36×** | **~2 min** |
| 16 KiB | 0.070 | 43× | ~8 min |
| never | 0.066 | 45× | everything since boot |

4 KiB is where the returns stop: 16 KiB buys 20 % more and costs four times the log.
Note this is **not** madflight's 32-sector constant, which is right for a drone whose data
rate fills a sector in milliseconds; at 34 B/s a sector is fifteen seconds and the
arithmetic lands somewhere else.

**The asymmetry between the two methods is the point.** `writeRaw()` is how the `FMT`
preamble reaches the card. A preamble that does not land makes the file unreadable rather
than merely short — every record in it becomes undecodable, not just the last few — so it
is worth a sync that records are not. This is also why the two methods exist separately at
all, the other reason being that `writeRaw()` cannot rotate and so cannot re-enter the
`onOpen` callback.

**Alternative considered: our own 512 B buffer, as madflight has.** Rejected: the
`SdVolume` cache block already is that buffer, and a second one would cost 512 B of a
3184 B headroom to duplicate something the library does for free. madflight needs its own
because it writes byte at a time into SdFat; we hand whole records to `SdFile::write()`,
which fills the cache block itself.

### Nothing pads the tail, and nothing scans on reopen

An earlier sketch of this change carried both: `0xFF` padding so a half-filled sector
self-terminates, and a forward scan at `begin()` to recover blocks written past a stale
`fileSize`. Flushing on a byte count rather than on a timer removes the need for both,
because every flush syncs `fileSize` — so the only thing at risk is the partial block
still in the cache, and there is never data on the card beyond what the directory entry
admits to. Nothing is left invisible and there is nothing to recover.

### The reader's view of a reused file does not change

Rotation deletes the next slot before opening it, so a file always starts empty and never
holds records from a previous lap after the write cursor. Making rotation frequent
therefore introduces no ambiguity for a reader — the problem a circular log usually has
does not exist here, and the price of not having it is exactly the `SD.remove()` this
design has to measure.

## Risks / Trade-offs

**Rotation now happens, and its duration is set by the card** → The FAT work can be
estimated — clusters = file size / cluster size, FAT sectors = clusters ÷ 128 for FAT32, and
about `1 + fatCount()` sector operations each — which at 1 MiB is single digits for any
cluster size. But that is a **floor, not a bound**, twice over. It assumes the chain's
entries sit in consecutive FAT sectors, and a fragmented file can visit one sector per
cluster; and it says nothing about the card's own write latency, which varies by an order of
magnitude between cards. Both are why `tasks.md` measures a rotation rather than deriving
one. `test_report_sd_volume_geometry` in the Unity suite reports the cluster size, the
contiguous estimate and the fragmented ceiling, so the gap between the last two is visible
rather than assumed away. At 1 MiB even the ceiling is a few dozen operations, which is the
reason this risk is acceptable at the new size and was not at the old one.

**Shrinking the files makes rotation cheaper, not dearer** → Worth stating because the
backlog entry this change comes from once claimed the opposite. The latent watchdog risk
belongs to the 1 GiB default, where a delete can be thousands of sector operations against
a 1398 ms timeout, and it is latent only because rotation needs about a year to arrive.
This change removes that risk in the act of exercising the path.

**A power cut now costs ~2 minutes of log instead of one record** → Accepted, declared in
the spec, and the reason the requirement was renamed rather than quietly reworded. Two
minutes of housekeeping does not change a conclusion about a stack margin, which is what
the log exists for.

**The bound is a byte count, so it is only ~2 minutes at the current write rate** → If a
future change adds records, the same 4 KiB becomes a shorter interval, which is the safe
direction. If the rate ever falls, the interval lengthens; the spec states the bound as a
declared quantity rather than as a duration for that reason.

**Nothing else may touch the card between flushes** → The unflushed data sits in the shared
`SdVolume` cache block, so any other card access evicts it early. That is harmless — an
early flush is just a write — but it means the figures above assume `src/sdwrite.cpp`
remains the only code touching the card. It is, and the ownership rule is what keeps it
that way.

**`index.bin` stops being dead code, and it has never been exercised in flight** → Every
rotation now opens, seeks, writes, flushes and closes a second file. That is the one place
this change makes the firmware do something it has genuinely never done on a board doing
its job, and it is where a defect would hide.

## Migration Plan

A card written by the current firmware is read identically by the new one: the format does
not change and the file names do not change. What changes is that files stop growing past
1 MiB, so a card already holding a multi-gigabyte `data0.BIN` keeps it, and the next
rotation deletes it when the ring comes round. Nothing has to be erased by hand.

Rollback is reverting the commit. The card is left holding smaller files than the old
default would have made, which the old firmware reads without complaint.

## Open Questions

None that can be deferred. The cluster size of the card in use is unknown but is a
measurement `tasks.md` takes, not a decision this design rests on: the conclusion holds for
every cluster size in the table, and the measurement exists to confirm the card is not
something unexpected.
