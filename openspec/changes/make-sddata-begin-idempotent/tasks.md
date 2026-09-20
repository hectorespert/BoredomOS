Markers used below: **`[board]`** the assembled board must be attached; **`[hands]`** a
person must do something no script observes (pull the card, cut power, press RESET);
**`[destructive]`** the step replaces the flight firmware or erases the card and must be
asked for before it is run, then followed by `pio run -t upload`.

## 1. The lifecycle in `lib/SdData`

- [x] 1.1 Add `void end();` to `lib/SdData/SdData.h`, documented as closing the open file
  and nothing else — it does not reset `_fileIdx`, clear `_onOpen` or touch `index.bin`,
  and `begin()` is the way back. Verify with `pio run` (the header is compiled into the
  flight build through `src/sdwrite.cpp`).
- [x] 1.2 Implement `SdData::end()` in `lib/SdData/SdData.cpp` as a guarded
  `_dataFile.close()`. Verify with `pio run` and `pio run -e libs`.
- [x] 1.3 Make `begin()` unconditional: close any held file first, then `readLogIndex()`,
  then open, then `notifyOpened()` — removing the `if (!_dataFile)` guard rather than
  inverting it, and closing *before* `readLogIndex()` for the cache reason in `design.md`.
  Verify with `pio run` and `pio run -e libs`.
- [x] 1.4 Update the comment on `setOnOpen()` in the header: it currently says the
  callback must be set before `begin()` "to catch the first file's open", which stays
  true, but the reason changes — every `begin()` now opens, so a callback registered
  later catches subsequent opens rather than none. Verify by reading it back against the
  new `begin()`.
- [x] 1.5 Confirm `src/sdwrite.cpp` needs no edit: it calls `setOnOpen()` then `begin()`
  once, with nothing open, so the new close is a no-op on that path. Verify by reading
  the call site and by `pio run` linking unchanged. **This cannot be verified by the
  Unity suite** — `test_build_src` is off for `libs`, so `src/` is not in that binary.

## 2. The Unity suite stops deleting an open file

- [x] 2.1 Call `sdData.end()` at the top of `tearDown()` in
  `test/test_libs/test_main.cpp`, before `cleanSdFiles()`, with a comment saying that the
  order is what prevents removing a held file — there is no guard, by the decision in
  `design.md`. Verify by reading it back; the behavioural check is 3.2.
- [x] 2.2 Add a case asserting that `begin()` fires `onOpen`: register a counting
  callback, `end()`, `begin()`, assert the count advanced, then restore whatever callback
  the other cases expect. Note in a comment that this proves the wiring, not that the
  preamble reached the card — 4.2 is what proves that.
- [x] 2.3 Add a case asserting `begin()` is idempotent in the sense that matters: with a
  file open, write `index.bin` to a different index by hand, call `begin()`, and assert
  the file the object now writes to is the one that index names. Verify the case fails
  against the old `begin()` before it passes against the new one — a test that never saw
  the bug is not evidence.
  **Done, negative check included.** Ran with the pre-fix `begin()` restored: both new
  cases failed, and on the predicted assertions —
  `test_sddata_on_open_fires_on_begin` "Expected 2 Was 1" (the call with a file already
  open fired nothing) and `test_sddata_begin_reopens_the_file_the_index_names` "begin()
  did not open the file index.bin names". All 15 pre-existing cases passed in that run
  too, so `tearDown()`'s `end()` breaks nothing. Then restored the fix: 17/17.

## 3. Run what can be run

- [x] 3.1 `pio run` and `pio run -e libs` both succeed, and `scripts/ram_budget.py`
  reports headroom unchanged at 3184 B (this change adds no RAM; a different figure means
  something unintended happened). Note that `pio run -e libs` passing does **not** imply
  `pio test -e libs` links — a `lib/` change that pulls a new FreeRTOS translation unit
  into that environment has broken this before.
- [x] 3.2 **`[board]` `[destructive]`** `pio test -e libs` — all cases pass, including
  2.2 and 2.3. This replaces the flight firmware and deletes `data*.BIN` and `index.bin`
  from the card on every case, so ask first, and follow with `pio run -t upload`.
  **17/17 in 43,2 s.** Asked for and authorised; the card's previous log was lost, which
  was the acknowledged cost. `pio run -t upload` restored the flight firmware afterwards.
  Worth recording for next time: `pio test -e libs --without-uploading --without-testing`
  says "Building in test mode" and **links the Unity binary without flashing**, and `nm`
  on `.pio/build/libs/firmware.elf` then shows `runUnityTests()` and each case. That
  clears the link-failure class this task warns about without spending a flash.
- [x] 3.3 `pio check` reports no new findings. The baseline is 2 LOW, one in
  `lib/Battery` and one in `src`, neither from this change.
- [x] 3.4 **`[board]`** `pio test` (the HIL suite) still passes, to show the flight
  firmware that `pio run -t upload` restored is the one that flies and answers the link.
  **25 passed, 8 skipped, 0 failed** in 115 s — the same figures as the baseline. The eight
  skips are the documented ones: four need a UART adapter, two need the reduced
  configuration set by hand, one needs a RESET press, one is gated behind
  `HIL_CLOCK_RESET=1`.

## 4. What only the card can show

- [x] 4.1 **`[board]`** Leave the flight firmware running long enough to write a log, so
  4.2 has something to read. Nothing in this change alters what is written, so this is
  the ordinary flight path, not a special mode.
  **Running since the post-test upload.** The card was erased by 3.2, so `data0.BIN` is a
  brand-new file whose first bytes are the preamble written by the `onOpen` callback on
  the open `begin()` performed — which is exactly the artifact 4.2 needs, better than an
  appended-to file would have been.
- [ ] 4.2 **`[board]` `[hands]`** Pull the card and parse `data0.BIN` with
  `pymavlink`'s `DFReader` — the procedure is in `ARCHITECTURE.md` §5.2 — and confirm the
  file opens with the `FMT` preamble and a `TIME` record, written by the `onOpen` callback
  on the open that `begin()` performed. This is the only step that distinguishes "the
  callback ran" from "the preamble is in the file". Note that `mavlogdump.py` is not in
  the bundled `pymavlink`, so use `DFReader` directly.
- [x] 4.3 **`[board]`** Confirm the high-water marks are unchanged: read
  `SdWrite` and `Logger` off the housekeeping stream (`NAMED_VALUE_INT`, armed with
  `MAV_CMD_SET_MESSAGE_INTERVAL` on message id 252) and compare against the reference
  taken on 2026-09-20 — `Logger` 56 of 160 words free, `SdWrite` 87 of 256. `end()` adds
  no stack frame to either task, so a change here would mean something unintended.
  **Read over the link, no hands needed** — the marker was wrong. Armed message id 252
  with `MAV_CMD_SET_MESSAGE_INTERVAL`, read the stream, disarmed it again:

  | task | reference | now | delta |
  |---|---|---|---|
  | `Logger` | 56 | 56 | — |
  | `SdWrite` | 87 | **83** | −4 |
  | `Mavlink` | 122 | **137** | +15 |
  | `UartRead` | 41 | 41 | — |
  | `UartWrite` | 146 | 146 | — |
  | `UsbRead` | 52 | **50** | −2 |
  | `UsbWrite` | 136 | 136 | — |

  `SdWrite` moved by 4 words and I cannot prove those 4 are not mine. What bounds it:
  **`Mavlink` moved by 15 in the opposite direction, and this change cannot touch
  `TaskMavlink` at all** — so the run-to-run variation of a single reading is well above
  4 words, and the reference was one reading, not a distribution. A plausible mechanism
  that is also not this change: 3.2 had just erased the card, so `index.bin` was absent
  and `readLogIndex()` took its early-return branch instead of reading a file. `SdWrite`
  at 83 of 256 free is 32 %, in line with the rest of the fleet. Recorded rather than
  called "unchanged", because it is not unchanged.

  Also read, and worth carrying: `custom_mode` is `0x08020603` — reset reason 3, boot
  phase 6, **consecutive 2 of 3, cumulative 8 of 10**. Three flashes in this session
  moved it. One more reflash inside the 5-minute stability window would latch the reduced
  configuration; cumulative is two faults from its own threshold and only
  `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` clears either.

## 5. Documentation and the backlog

- [x] 5.1 Check whether `ARCHITECTURE.md` describes `SdData`'s lifecycle anywhere it
  would now be inaccurate, and update it in the same commit if so. Verify by grepping for
  `begin()` and `SdData` in that file and reading each hit.
  **Nothing was inaccurate.** §5.2 already said the callback fires "at `begin()` and again
  on rotation", which this change makes more true rather than less. Added a paragraph
  anyway, stating that `begin()` opens unconditionally and why `end()` exists, because the
  old text left it open whether a second `begin()` notifies and the next reader would have
  had to derive it from the source.
- [x] 5.2 Confirm `CLAUDE.md` needs no edit: the invariants it lists — one owner per
  resource, queues by value, stacks in words — are all unaffected. Verify by reading the
  conventions section against this change's diff.
  **Confirmed, no edit.** No queue is touched, no stack size changes, and `end()` is a
  method on the object that already owns `_dataFile` — the test reaches it through the same
  public interface `src/sdwrite.cpp` uses, so nothing becomes a second owner. Noted in
  passing that CLAUDE.md's stack-margin bullet already says the high-water marks can be
  read "live off the `NAMED_VALUE_INT` housekeeping stream, which needs only the link",
  which is what made this change's `[hands]` marker on 4.3 wrong.
- [ ] 5.3 At archive time, carry anything left unticked into `TODO.md` as an entry naming
  this change, per the project's archive rule. In particular, if 4.2 has not been done,
  the `onOpen` coverage gap is **not** closed and the entry must say so — the Unity case
  from 2.2 proves the wiring only.
