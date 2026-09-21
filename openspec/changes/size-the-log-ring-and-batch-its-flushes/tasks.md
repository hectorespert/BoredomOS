Markers: **`[board]`** the assembled board must be attached; **`[hands]`** a person must do
something no script observes; **`[power]`** power has to be physically removed mid-write;
**`[destructive]`** replaces the flight firmware or erases the card, so ask first and follow
with `pio run -t upload`.

**Depends on** the pull request *Report the card's FAT geometry from the Unity suite* having
landed: task 1.1 reads its output, and without it the cluster size is unknown.

## 1. Measure before changing anything

- [ ] 1.1 **`[board]` `[destructive]`** Run `pio test -e libs` and read
  `test_report_sd_volume_geometry`'s output: cluster size, FAT type, and the reported
  sector-operation cost of deleting 1 GiB and 1 MiB. Record the figures in this file. If
  deleting 1 GiB comes out above ~1000 sector operations, note it — that is the latent
  watchdog risk this change removes, and it is worth having the number on record before it
  is gone.
- [ ] 1.2 **`[board]` `[hands]`** With the **current** 1 GiB defaults still in place,
  measure how long one rotation takes. `TEST_FILE_SIZE_MB` is 1024 *bytes*, so the Unity
  rotation case cannot answer this; it needs a temporary build whose file size is large
  enough to matter, with `millis()` either side of the rotation, reported through
  `TEST_MESSAGE`. This is the number `design.md` deliberately does not derive, because card
  write latency varies by an order of magnitude. **If no practical file size makes this
  measurable, say so here and leave it unticked** rather than inventing a figure.
- [ ] 1.3 Record the baseline the flush change will be measured against: at 34 B/s the
  current policy is 3.0 sector writes/s by derivation. Note plainly that this is derived
  from reading `SdFile::sync()` and `SdVolume::cacheFlush()`, **not** measured — nothing on
  this board counts sector writes, and saying so is the point.

## 2. The ring's size

- [ ] 2.1 Change `SdData`'s constructor defaults in `lib/SdData/SdData.h` from
  `(4, 1 GiB)` to `(4, 1 MiB)`, with a comment giving the download-time derivation and
  pointing at `design.md`'s table. Verify with `pio run` — `src/sdwrite.cpp` constructs
  `SdData sdData;` with no arguments, so the new footprint arrives without an edit in
  `src/`, which is worth confirming by reading the link output rather than assuming.
- [ ] 2.2 Check `test/test_libs/test_main.cpp`'s `TEST_FILE_SIZE_MB` against the new
  default. The constant is `1024UL` and means bytes, which is listed in *Minor leftovers
  cleanup*; decide whether this change fixes the name or leaves it, and say which in the
  commit. Verify the rotation case still rotates.
- [ ] 2.3 Re-check `scripts/ram_budget.py`'s headroom. It should not move — the defaults are
  constructor arguments, not storage — and a change here would mean something unintended.

## 3. The flush policy

- [ ] 3.1 Add a byte counter to `SdData` and flush in `write()` once it reaches 4 KiB,
  resetting it on each flush and on every open. Document the interval against
  `design.md`'s table, and state in the header that the bound is a byte count and therefore
  a duration only at the current write rate. Verify with `pio run` and `pio run -e libs`.
- [ ] 3.2 Leave `writeRaw()` flushing unconditionally, and say in a comment why the
  asymmetry exists: it carries the `FMT` preamble, and a preamble that does not reach the
  card makes every record in the file undecodable rather than merely losing the last few.
  Verify by reading it back against `src/sdwrite.cpp`'s `writeLogPreamble()`.
- [ ] 3.3 Reset the counter in the rotation path too, so the interval does not straddle two
  files. Verify with a Unity case: write enough to rotate, and assert the new file's size
  advances rather than sitting at zero until the counter happens to fill.
- [ ] 3.4 Add a Unity case for the interval itself: write less than 4 KiB and assert the
  file on the card is shorter than what was written; write past 4 KiB and assert it catches
  up. **Verify this case fails against the current per-record flush before it passes**
  against the new one — a test that never saw the old behaviour is not evidence.

## 4. Run what can be run

- [ ] 4.1 `pio run` and `pio run -e libs` succeed and headroom is unchanged at 3184 B.
- [ ] 4.2 `pio test -e libs --without-uploading --without-testing` links the Unity binary,
  confirmed with `nm` on `.pio/build/libs/firmware.elf`. This catches the link-failure class
  `pio run -e libs` misses and costs no flash.
- [ ] 4.3 **`[board]` `[destructive]`** `pio test -e libs` — all cases pass, including 3.3
  and 3.4.
- [ ] 4.4 `pio check` reports no new findings against the 2 LOW baseline.
- [ ] 4.5 **`[board]`** `pio test` (the HIL suite) still passes at 25/8/0 after
  `pio run -t upload` restores the flight firmware.

## 5. What only the board and the card can show

- [ ] 5.1 **`[board]`** Leave the flight firmware running for **at least 35 hours**, so the
  ring comes all the way round and every file is reused at least once. This is the first
  time rotation runs on a board doing its actual job. Shorter is not enough: ~8.6 h only
  reaches the first rotation.
- [ ] 5.2 **`[board]`** During and after 5.1, confirm the spec's *Maintaining the log never
  resets the board*: the reset reason in the heartbeat's `custom_mode` is unchanged, time
  since boot increased monotonically across every rotation, and the fault counters are no
  higher than they started. A watchdog reset during a rotation is the failure this looks
  for, and it would otherwise look like a mystery.
- [ ] 5.3 **`[board]` `[hands]`** Pull the card and confirm the ring: four files, each about
  1 MiB, `index.bin` present and naming one of them, and each file parseable on its own with
  `DFReader` — the procedure is in `ARCHITECTURE.md` §5.2. This is also the first evidence
  that `index.bin` is written correctly by a rotation that really happened.
- [ ] 5.4 **`[board]` `[hands]` `[power]`** Cut power mid-write, then read the card and
  measure what is missing from the end of the last file. Confirm it is a contiguous stretch
  no larger than 4 KiB, which is the spec's declared bound. **No script observes this** and
  it is the only evidence for the requirement this change modifies.
- [ ] 5.5 **`[board]` `[hands]` `[power]`** Repeat 5.4 on a board that has been logging for
  minutes rather than hours, and confirm the missing stretch is of the same order. That is
  the spec's *The amount at risk does not grow with uptime* scenario, and it is what
  distinguishes a bounded policy from one that happens to look fine on a short run.
- [ ] 5.6 **`[board]`** Read the high-water marks off the housekeeping stream and compare
  against whatever baseline exists by then. Note that *Finish what
  make-sddata-begin-idempotent left open* records that the current reference figures are
  single samples and cannot support a claim of "unchanged"; if that entry has not been done,
  say so here rather than comparing against one reading.

## 6. Documentation and the backlog

- [ ] 6.1 Update `ARCHITECTURE.md` §5.2 in the same commit: the ring's defaults, the flush
  policy and what a power cut now costs. The section currently states the 1 GiB default and
  says rotation does not happen within a realistic mission, which this change makes false.
- [ ] 6.2 Check whether `CLAUDE.md` needs anything. The invariants it lists look unaffected,
  but the destructive-test paragraph mentions what `cleanSdFiles()` deletes and that is
  worth re-reading against a ring that actually rotates.
- [ ] 6.3 Delete *[The default SD ring is 4 GiB and never rotates]* and *[The flush policy
  costs far more card writes than it needs to]* from `TODO.md` in the proposing commit, and
  re-point every entry that cross-references either at this change id. At least *[Download
  the flight log over the MAVLink log protocol]*, *[Serve the SD card over MAVLink FTP]*,
  *[Replace `arduino-libraries/SD` with `greiman/SdFat`]*, *[Put the ring's position in the
  log data instead of `index.bin`]* and *[Minor leftovers cleanup]* refer to one or both.
- [ ] 6.4 At archive time, carry anything left unticked into `TODO.md` naming this change.
  5.1 through 5.5 are the likely ones: they need 35 hours of uptime and two power cuts, and
  **if 5.4 and 5.5 are not done, the requirement this change modified has no evidence at
  all** — the entry must say that in those words.
