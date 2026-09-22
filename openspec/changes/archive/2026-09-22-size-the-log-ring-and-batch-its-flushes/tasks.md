Markers: **`[board]`** the assembled board must be attached; **`[hands]`** a person must do
something no script observes; **`[power]`** power has to be physically removed mid-write;
**`[destructive]`** replaces the flight firmware or erases the card, so ask first and follow
with `pio run -t upload`.

**Depends on** the pull request *Report the card's FAT geometry from the Unity suite* having
landed: task 1.1 reads its output, and without it the cluster size is unknown.

## 1. Measure before changing anything

- [x] 1.1 **`[board]` `[destructive]`** Run `pio test -e libs` and read
  `test_report_sd_volume_geometry`'s output: cluster size, FAT type, and the reported
  sector-operation cost of deleting 1 GiB and 1 MiB. Record the figures in this file. If
  deleting 1 GiB comes out above ~1000 sector operations, note it — that is the latent
  watchdog risk this change removes, and it is worth having the number on record before it
  is gone.

  **Measured 2026-09-21, 18/18 cases passing:**

  ```
  FAT32: cluster=32768 B (64 blocks) clusters=242304 fatBlocks=1894 fats=2
  card=15523840 blocks (7580 MiB)
  deleting 1048576 KiB: 32768 clusters, 256 FAT sectors, ~768 ops contiguous, <=98304 fragmented
  deleting 1024 KiB: 32 clusters, 1 FAT sectors, ~3 ops contiguous, <=96 fragmented
  ```

  The contiguous figure for 1 GiB is ~768, below the ~1000 this task set as its threshold.
  **The fragmented ceiling is 98 304**, and that is the number that matters: at a
  conservative 0.5 ms per sector operation it is about **49 seconds** against a
  `WDT_TIMEOUT_MS` of 1398. So the risk is real rather than theoretical, and it is a
  property of fragmentation rather than of the card being unusual — 32 KiB clusters on an
  8 GB FAT32 card is exactly what the SD Association's formatter produces.

  At 1 MiB the ceiling is 96 operations, about 48 ms. **Safe at the fragmented ceiling, not
  merely at the contiguous floor**, which is a stronger result than this task asked for.

  **`pio test` hides `TEST_MESSAGE` output — `pio test -v` is required.** Plain `pio test`
  prints only the `[PASSED]`/`[FAILED]` result lines, so the figures above are invisible
  through the command `CLAUDE.md` documents. That applies to the two pre-existing
  report-only cases as well: `test_report_r64cnt_range` and
  `test_report_internal_versus_ds1307_drift` have never shown their numbers through the
  documented invocation. Recorded as a backlog item by 6.2.

  Cost of this step, for the record: the card's previous log was erased, which was an
  explicit decision, and the flash cycles took the cumulative fault counter to 9 of 10
  before it was cleared with `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`. It now reads 1 of 10.
  Check it before a long series of reflashes rather than after.
- [ ] 1.2 **`[board]` `[hands]`** With the **current** 1 GiB defaults still in place,
  measure how long one rotation takes. `TEST_FILE_SIZE_MB` is 1024 *bytes*, so the Unity
  rotation case cannot answer this; it needs a temporary build whose file size is large
  enough to matter, with `millis()` either side of the rotation, reported through
  `TEST_MESSAGE`. This is the number `design.md` deliberately does not derive, because card
  write latency varies by an order of magnitude. **If no practical file size makes this
  measurable, say so here and leave it unticked** rather than inventing a figure.
- [x] 1.3 Record the baseline the flush change will be measured against: at 34 B/s the
  current policy is 3.0 sector writes/s by derivation. Note plainly that this is derived
  from reading `SdFile::sync()` and `SdVolume::cacheFlush()`, **not** measured — nothing on
  this board counts sector writes, and saying so is the point.

  **Recorded, and partly corroborated by accident.** The 3.0 sector writes/s figure is
  derived from reading `SdFile::sync()` and `SdVolume::cacheFlush()`; nothing on this board
  counts sector writes, so it is not measured and is not presented as such. What *was*
  measured: the Unity suite went from **43 s to 23 s** on the same cases once `write()`
  batched. That suite is dominated by SD writes in tight loops, so the ~1.9x there is not
  the flight ratio — at 34 B/s the directory write dominates and the derivation gives 36x —
  but it is direct evidence that the number of card operations fell, from a clock rather
  than from a reading of the library.
## 2. The ring's size

- [x] 2.1 Change `SdData`'s constructor defaults in `lib/SdData/SdData.h` from
  `(4, 1 GiB)` to `(4, 1 MiB)`, with a comment giving the download-time derivation and
  pointing at `design.md`'s table. Verify with `pio run` — `src/sdwrite.cpp` constructs
  `SdData sdData;` with no arguments, so the new footprint arrives without an edit in
  `src/`, which is worth confirming by reading the link output rather than assuming.
  **Confirmed: `src/` did not move.** `git diff --stat` shows no file under `src/`, and
  the link output is unchanged apart from the 4 bytes in 2.3. `src/sdwrite.cpp` constructs
  `SdData sdData;` with no arguments, so the footprint change arrives through the header.

- [x] 2.2 Check `test/test_libs/test_main.cpp`'s `TEST_FILE_SIZE_MB` against the new
  default. The constant is `1024UL` and means bytes, which is listed in *Minor leftovers
  cleanup*; decide whether this change fixes the name or leaves it, and say which in the
  commit. Verify the rotation case still rotates.
  **Renamed to `TEST_FILE_SIZE_BYTES` and raised to 6144, and the value is constrained from
  two sides rather than chosen.** It must be LARGER than the 4 KiB flush interval or a
  rotation always arrives first and `close()` syncs, so **the suite cannot exercise the
  batching path at all** — which is what the old 1024 did. And it must NOT be a MULTIPLE of
  the interval or the rotation lands straight after a sync with no pending tail, which is
  what 8192 did and is recorded under 3.3. 1.5x satisfies both. The two rotation loops now derive
  their count from the constant (`TEST_FILE_SIZE_BYTES / sizeof(payload)` per outer
  iteration), so they still cross every file of the ring. The rename closes the
  `TEST_FILE_SIZE_MB` item in *Minor leftovers cleanup*; 6.2 removes it there.
- [x] 2.3 Re-check `scripts/ram_budget.py`'s headroom. It should not move — the defaults are
  constructor arguments, not storage — and a change here would mean something unintended.

  **It moved by 4 bytes: 3184 -> 3180 B.** That is the `size_t _sinceFlush` member and
  nothing else, which is what `proposal.md` said the only new state would be. Recorded as a
  movement rather than reported as unchanged — the task expected zero and the honest answer
  is four.
## 3. The flush policy

- [x] 3.1 Add a byte counter to `SdData` and flush in `write()` once it reaches 4 KiB,
  resetting it on each flush and on every open. Document the interval against
  `design.md`'s table, and state in the header that the bound is a byte count and therefore
  a duration only at the current write rate. Verify with `pio run` and `pio run -e libs`.

- [x] 3.2 Leave `writeRaw()` flushing unconditionally, and say in a comment why the
  asymmetry exists: it carries the `FMT` preamble, and a preamble that does not reach the
  card makes every record in the file undecodable rather than merely losing the last few.
  Verify by reading it back against `src/sdwrite.cpp`'s `writeLogPreamble()`.

- [x] 3.3 Reset the counter in the rotation path too, so the interval does not straddle two
  files. Verify with a Unity case: write enough to rotate, and assert the new file's size
  advances rather than sitting at zero until the counter happens to fill.
  **Reframed, because the original intent turned out to be untestable.** The task asked to
  assert the counter is reset at a rotation. It is reset — but carrying it across a rotation
  would only make the new file's first interval *shorter*, which is a smaller bound rather
  than a violated one, and nothing observable. So there is no failing behaviour to catch.
  What the case asserts instead is the thing that could genuinely break:
  `test_sddata_rotation_does_not_lose_unsynced_bytes` checks that the file a rotation closes
  reports **everything** written to it, not merely what had been synced when the last
  interval elapsed. It passes against the old per-record flush too, so it is a regression
  guard rather than a bug-demonstrating test, and that is said in the case's own comment.

  **And the first version of it was vacuous — found by Copilot, confirmed by arithmetic.**
  `TEST_FILE_SIZE_BYTES` was 8192, an exact multiple of the 4096-byte interval, so the
  rotation landed immediately after a sync with **zero** bytes pending: the case asserted a
  tail survived when there was no tail. Simulating both sizes:

  ```
  file=8192: at rotation fileSize=8192 lastSync=8192 pending=0
  file=6144: at rotation fileSize=6144 lastSync=4096 pending=2048
  ```

  The constant is 6144 now — 1.5x the interval, so larger than it *and* not a multiple of
  it, which are two separate requirements the comment on the constant now states. The case
  also asserts the tail exists before relying on it (`visibleBefore < written`), so it
  fails loudly rather than silently proving nothing if either constant moves again.
- [x] 3.4 Add a Unity case for the interval itself: write less than 4 KiB and assert the
  file on the card is shorter than what was written; write past 4 KiB and assert it catches
  up. **Verify this case fails against the current per-record flush before it passes**
  against the new one — a test that never saw the old behaviour is not evidence.

  **Done, and watched failing first.** Against the per-record flush restored temporarily,
  `test_sddata_write_batches_its_flushes` failed on
  `"write() synced before reaching its interval"` — the predicted message — while the other
  19 cases passed. With the batching restored: 20/20.
## 4. Run what can be run

- [x] 4.1 `pio run` and `pio run -e libs` succeed and headroom is **3180 B** — the task was
  written expecting 3184 unchanged, and the measured figure is four bytes lower.
  **Both SUCCESS. The four bytes are `_sinceFlush` and nothing else; see 2.3.** The
  acceptance text is corrected here rather than left as written, because a checked-off task
  stating a figure the build does not produce is the same defect this project has been
  clearing out of its documentation all week.
- [x] 4.2 `pio test -e libs --without-uploading --without-testing` links the Unity binary,
  confirmed with `nm` on `.pio/build/libs/firmware.elf`. This catches the link-failure class
  `pio run -e libs` misses and costs no flash.
  **Links.** `nm` shows both new cases in `.pio/build/libs/firmware.elf`.
- [x] 4.3 **`[board]` `[destructive]`** `pio test -e libs` — all cases pass, including 3.3
  and 3.4.
  **20/20 in 23 s**, including 3.3 and 3.4's cases. Followed by `pio run -t upload`.
- [x] 4.4 `pio check` reports no new findings against the 2 LOW baseline.
  **2 LOW, unchanged**, one in `lib/Battery` and one in `src`, neither from this change.
- [x] 4.5 **`[board]`** `pio test` (the HIL suite) still passes at 25/8/0 after
  `pio run -t upload` restores the flight firmware.

  **25 passed, 8 skipped, 0 failed** — the baseline figures.
  **It failed on the first attempt, and the cause is worth recording because it was not this
  change.** Six flashes in a few minutes, none surviving the five-minute stability window,
  drove the consecutive fault counter to 3 of 3 and **latched the reduced configuration**
  (`custom_mode` `0x05030603`, `state=5`, `base_mode=0x80` without `AUTO_ENABLED`). In that
  configuration `TaskMavlink` withholds `BATTERY_STATUS`, so
  `check_telemetry.py`'s `test_battery_status_every_2s` reported 0.00 Hz and failed, while
  `check_recovery.py`'s `test_heartbeat_reports_operational` self-skipped correctly. That is
  exactly the backlog entry *[`check_telemetry.py`'s `test_battery_status_every_2s` assumes
  the normal configuration]*, firing for the first time — one case self-skips on that
  condition and its neighbour does not.
  `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` cleared both counters and restored the normal
  configuration immediately, without needing to wait out the window, after which the suite
  passed. **Lesson for anyone applying the rest of this change: check `custom_mode` before
  a series of reflashes, not after.**
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

- [x] 6.1 Update `ARCHITECTURE.md` §5.2 in the same commit: the ring's defaults, the flush
  policy and what a power cut now costs. The section currently states the 1 GiB default and
  says rotation does not happen within a realistic mission, which this change makes false.
  **Done.** The ring paragraph now carries the new defaults with the coverage and download
  figures, the delete-then-open property and what it costs, the measured geometry, the
  batching and the power-loss bound. Also records that the report-only cases need
  `pio test -e libs -v`.
- [x] 6.2 Check whether `CLAUDE.md` needs anything. The invariants it lists look unaffected,
  but the destructive-test paragraph mentions what `cleanSdFiles()` deletes and that is
  worth re-reading against a ring that actually rotates.
  **CLAUDE.md did need something, twice over.** Added that `pio test` hides `TEST_MESSAGE`
  so the report-only cases need `-v` — which cost time in 1.1 — and that `custom_mode`
  should be checked *before* a series of reflashes, because a latched reduced configuration
  makes `check_telemetry.py` fail in a way that reads as a regression in whatever is being
  worked on. That is what happened in 4.5. The `cleanSdFiles()` paragraph itself is still
  accurate against a ring that rotates. Also removed the `TEST_FILE_SIZE_MB` item from
  *Minor leftovers cleanup* in TODO.md, closed by 2.2.
- [x] 6.3 Delete *[The default SD ring is 4 GiB and never rotates]* and *[The flush policy
  costs far more card writes than it needs to]* from `TODO.md` in the proposing commit, and
  re-point every entry that cross-references either at this change id.
  **Done in the proposing commit**, which is what the project's rule asks for — the entries
  are deleted so nothing is described twice, and five entries were re-pointed here:
  *[Download the flight log over the MAVLink log protocol]*, *[Serve the SD card over
  MAVLink FTP]*, *[Replace `arduino-libraries/SD` with `greiman/SdFat`]*, *[Put the ring's
  position in the log data instead of `index.bin`]* and *[Minor leftovers cleanup]*. Two of
  those five were wrapped across lines and a first pass missed them; all 22 cross-references
  in the file now resolve. Ticked here rather than left open because leaving it unticked
  would read as pending work at archive time, when the only thing it could mean is that the
  proposing commit had not happened.
- [x] 6.4 At archive time, carry anything left unticked into `TODO.md` naming this change.
  5.1 through 5.5 are the likely ones: they need 35 hours of uptime and two power cuts, and
  **if 5.4 and 5.5 are not done, the requirement this change modified has no evidence at
  all** — the entry must say that in those words.

  **Done in the archiving commit.** *Finish what size-the-log-ring-and-batch-its-flushes
  left open* in `TODO.md` carries all seven: 1.2 (left unmeasured, as the task allowed),
  and 5.1–5.6, with 5.4 stated in those words — no script observes the loss-bound cut and
  until it runs the modified requirement has no supporting evidence at all. 5.6 also notes
  its own dependency on *make-sddata-begin-idempotent*'s 4.3 for a usable baseline.
