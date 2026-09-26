# Tasks

**[BOARD]** needs the assembled board on USB. **[HANDS]** also needs someone at it: pulling
the card, an adapter on D0/D1, or putting the board in the reduced configuration. Without the
board, only `pio run` on both environments, the CI greps and `pio check` run, and none of them
says whether a task body works. Keep the host awake (`caffeinate -i`) for every HIL run: a
sleeping host shows up as cadence failures that are not real.

## 1. Spike: can the active file be read while it is written

- [x] 1.1 **[BOARD]** On a throwaway branch, not merged, add to `TaskSdWrite` a read of the
      active file through a second `FILE_READ` handle: open, record `size()`, read it in 90-byte
      pieces while records keep arriving at 1 Hz, close; repeat every 10 s for 15 min. Keep a
      running CRC-32 of every byte written to that file since it was opened, and compare it with
      the CRC-32 of the bytes read up to the recorded `size()` (the writer's CRC taken at the
      same length). Report counts and mismatches over the link. Verify: no mismatch in any pass.
      The card stays in the board; it cannot be pulled for now.
- [x] 1.2 **[BOARD]** In the same run, log `size()` of the read handle against the bytes
      written, and read newlib's free heap (`mallinfo`) before and after the 15 minutes.
      Verify: the reported size moves only at a sync or stays at the open value, never past the
      synced length, and free heap is the same at the end as after the first open/close.
      **Result (1.1 and 1.2)**, spike code in the working tree, never committed, reverted
      afterwards, `master` reflashed. 15-minute run, card in place, on a file that had just
      rotated (it started at 372 B, the preamble): **89 passes, 0 CRC mismatches.** The read
      handle's `size()` equalled the last synced length on every pass (`SpkLag` 0) while up to
      ~4 KiB had been written but not yet synced (`SpkBehind` 0-4094), so the handle ends at the
      synced size exactly as the design assumes. newlib bytes in use after each close: 284 on
      every pass, no growth. A pass over a 29 KB file took up to 211 ms, about 140 KB/s,
      interleaved with the log. `TaskSdWrite`, given 384 words for the spike, reached 157 free
      (227 used) with a 90 B local and the read path; `master` uses 183 of 256. The final
      design needs about 28 more words for a 112 B `LinkMsg` local, so its stack must grow to
      about 320 words, which belongs in 2.1's RAM figures.

- [x] 1.3 **[BOARD]** At the end of the run, close the writer's handle (`SdData::end()`),
      reopen the file read-only and CRC it whole against the writer's running CRC; then reset
      the board and confirm `begin()` reopens the same file, the size carries on growing, and a
      read of the whole file after the remount still matches the CRC of what was written before
      the reset. Verify: all three agree. This is the same library reading its own writes, so it
      catches damaged content but **does not prove the FAT and directory are coherent** — 1.4
      does.
      **Remount half, run after `persist-the-log-ring-position`** (instrumentation again in the
      working tree only, reverted, `master` reflashed): boot A reopened a part-filled file of
      256561 B — resuming, not a full 1 MiB file — logged for 3 min, closed at 263162 B with a
      whole-file read matching the writer's CRC (`SpkEnd` 1); after a commanded reset, boot B's
      `begin()` found **263162 B, CRC `0xa42db455`, identical to what A closed.** Ticked on that
      basis; the FAT's own coherence still rests on 1.4.

      **First run, before the fix:** The close/reopen half passed twice: after `SdData::end()`,
      a whole-file read matched the writer's running CRC (`SpkEnd` 1; 31067 B and 6479 B). The
      remount half **could not be run, because it exposed a pre-existing defect** — since
      fixed by `persist-the-log-ring-position`, so the remount half can now be run: `begin()`
      does not reopen the file that was being written. `writeLogIndex()` opens `index.bin` with
      `FILE_WRITE`, which includes `O_APPEND`, so its `seek(0)` is overridden and each index is
      appended; `readLogIndex()` reads the first four bytes, the first index ever written.
      Observed on three consecutive boots: each reopened the same full 1 MiB file
      (1052442 B, then 1053214 B, one preamble longer per boot) and rotated on its first
      write, **deleting the slot the previous boot had logged into.** Until that is fixed, a
      remount check has nothing meaningful to compare, and every reset destroys the log
      written since the reset before.

- [ ] 1.4 **[HANDS]** When the card can be pulled — it cannot at the time of writing — take it
      out after 1.1–1.3, mount it on another machine, run a filesystem check and parse every
      `data*.BIN` with `DFReader`. Verify: no filesystem error, and every file parses end to end.
      Until this runs, 1.3's outcome rests on the library alone; leave it unticked, and do not
      archive without either running it or carrying it into `TODO.md`.
- [x] 1.5 Record the outcome here and decide. If 1.1, 1.2 or 1.3 fails, the active file is not
      offered: change the second requirement of the spec delta to closed files only and the
      design's "active file" decision to its fallback, **before** section 3. Verify by reading
      the spec and design against the outcome.

      **Decision: the active file is offered, as specified.** 1.1-1.3 passed; nothing in the spec
      or design changes. The one thing still unproven is 1.4, an independent check of the card,
      which waits for the card to be removable.

## 2. Measure, then the data types

- [x] 2.1 **[BOARD]** Baseline: flash `master`, arm housekeeping, record every task's stack
      high-water mark, and the `RAM budget` lines. Then grow `LinkMsg` (the two new kinds, the
      `static_assert`), move both write queues to depth 9 and `sdWriteQueue` to 6, and read
      `RAM budget` again. Verify the figures are recorded here; if headroom falls below 1600 B,
      stop and bring the mailbox alternative back to a decision before going on.

      **Result.** Baseline on `master` (words free): `Mavlink` 129, `UartWrite` 140, `UsbWrite`
      130, `UartRead` 41, `UsbRead` 52, `Logger` 56, `SdWrite` 87 of 256; `committed 29728 B`,
      `headroom 3040 B`. With `LinkMsg` at 112 B, both write queues at depth 9, `sdWriteQueue` at
      6 and `TaskSdWrite` at 320 words (the spike's figure): **`committed 31168 B` (95.1 %),
      `headroom 1600 B`** — 1440 B, as estimated. Stacks after the resize, before any new code
      path: `Mavlink` 105, `UartWrite` 119, `UsbWrite` 118, `SdWrite` 151 of 320; none needs to
      grow. The result sits exactly on the stop threshold and leaves 576 B above the 1024 B
      floor; the mailbox alternative was put back to the user, who chose to continue with this
      design knowing that.
- [x] 2.2 Add the three `SdRecordKind`s (list, read, end) to `include/SdRecord.h` within its
      32 B `static_assert`, and rewrite the depth derivation beside the write queues in
      `src/main.cpp` to include the two chunks. Verify `pio run` on both environments with no
      warning.

      **Result.** Three kinds (`LogList`, `LogRead`, `LogEnd`) with one request payload;
      `SdRecord` stays 32 B under its `static_assert`. The depth derivations beside both queues
      and the `TaskSdWrite` stack say why they changed. Both environments build clean.
- [x] 2.3 Add `SdData`'s current-file-index getter. Verify `pio run -e libs` builds; this is a
      `lib/` change, so it also needs `pio test -e libs`, **which is destructive**: ask before
      running it and follow with `pio run -t upload`.

      **Result.** `currentFile()` and `fileCount()`. `pio test -e libs` passed, run with the
      user's go-ahead; the flight firmware was reflashed after it. It deleted the log on the
      card as expected. An attempt to download that log first, to keep it, sent its request
      right after opening the port without waiting for a `HEARTBEAT`, received no `LOG_ENTRY`,
      and was not checked before the suite ran, so that log is lost. Listing with a `HEARTBEAT`
      wait works on the reflashed board, twice out of two; the harness side is the likely cause,
      not proven.

## 3. Serving the log

- [x] 3.1 In `src/mavlink.cpp`: forward the three requests to `sdWriteQueue` when at least two
      slots are free; answer them directly when `taskSdWriteHandler` is `NULL`; give `LOG_ERASE`
      an empty case; pack `LOG_ENTRY` and `LOG_DATA` in `mavlinkPack`. Verify `pio run`.

      **Result.** Built on both environments; `pio check` still the same 2 LOW findings.
- [x] 3.2 In `src/sdwrite.cpp`: the listing (sizes, the `TIME` at offset 356, `time_utc` 0 when
      its source is *none*), the per-port stream state, one read handle at a time, one chunk per
      pass after the queue is drained, the pacing check, and ending a stream whose slot the ring
      has reused. Verify `pio run` on both environments and `pio check` with no new finding.

      **Result.** Built on both environments, `pio check` unchanged. **The download state
      (per-port stream and listing, the retained `File`) costs 92 B more than 2.1 measured:
      `headroom 1508 B`**, below the 1600 B the decision in 2.1 was taken on. Two details the
      design left open, settled here: a listing and a new request each open a second handle
      briefly (for a size, or the 16-byte head `TIME` record) and close it before returning, so
      at most two `SdFile`s are allocated at once, never more; and `LOG_ENTRY`s are paced like
      chunks.
- [x] 3.3 **[BOARD]** Smoke test with MAVProxy: `log list`, then `log download latest`. Verify
      the file downloads and `DFReader` parses it.


      **Result.** MAVProxy (`--nowait`, commands on stdin): `log list` printed `Log 1 numLogs 1
      lastLog 1 size 298320` with the file's date; `log download latest` wrote a 298320 B
      `log1.bin` — the whole active file. `DFReader` parses it end to end (44 FMT, 13 TIME,
      8652 SYS, 4328 PWR: eleven boots appended to this file today). A second download of the
      same range with pymavlink over USB took 5 s (about 60 KB/s) and is byte-identical. Only one
      file exists on the card: the Unity runs of `persist-the-log-ring-position` restarted the
      ring at `data0`.
## 4. HIL coverage

- [x] 4.1 **[BOARD]** A new `check_log_download.py`: listing (ids, `num_logs`,
      `last_log_num`, stable ids across two listings); a range request returning ten chunks at the
      right offsets; an absent id answered with `count` 0; `LOG_REQUEST_END` stopping the stream;
      `LOG_ERASE` changing nothing and emitting nothing; `HEARTBEAT`, `SYSTEM_TIME` and
      `SYS_STATUS` at 1 Hz during a whole-file download. Verify they pass.

      **Result.** Nine cases; eight pass on the board over USB, the reduced-configuration one
      self-skips (it is 4.4's). The cadence case checks `HEARTBEAT`, `SYSTEM_TIME` and
      `SYS_STATUS` at about 1 Hz through a whole download of the active file (~5 s over USB).

- [ ] 4.2 **[BOARD]** Download a closed file and the active file whole, noting when each
      download started and ended; then download the active file again. Verify with `DFReader`:
      every download parses; the two downloads of the active file agree over the length of the
      first; and the second one's once-per-second `SYS` record has no gap between the recorded
      times. Record the download times on UART and on USB.

      **Half run, left unticked.** The active file: downloaded whole (323004 B in 5.3 s over
      USB), then again 130 s later; the first download is a byte-for-byte prefix of the
      second, and the second's `SYS` records from the last boot, over the first download's
      window, are 9 records spaced exactly 1015 ms — the logger's own period throughout that
      boot, so the download cost the log nothing. **Not run: a closed file.** None exists: the
      Unity runs of `persist-the-log-ring-position` restarted the ring at `data0`, and the next
      rotation is hours of logging away. UART timing not measured either: no adapter.

- [ ] 4.3 **[HANDS]** When the card can be pulled, compare 4.2's downloads byte for byte with
      the files on the card. Deferred with 1.4 for the same reason; the same rule applies.
- [x] 4.4 **[BOARD][HANDS]** Put the board in the reduced configuration without removing the
      card (three consecutive boots that fail the stability window, as `fault-recovery`
      describes), then list and request data for id 1. Verify `num_logs` 0 and a single
      `LOG_DATA` with `count` 0; then clear the counters with
      `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`. This exercises the same no-`TaskSdWrite` path as a
      missing card; the missing-card scenario itself waits for the card to be removable.

      **Result.** Needed no hands: three flashes in a row are three boots inside the stability
      window, and the third came up in the reduced configuration (`custom_mode` `0x05030603`,
      `MAV_STATE_CRITICAL`). `test_the_reduced_configuration_reports_no_logs` passed: `num_logs`
      0, and one `LOG_DATA` with `count` 0. The counters were then cleared with
      `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` and the board came back normal (`0x01000003`).

- [x] 4.5 **[BOARD]** Run the whole HIL suite. Verify it passes as before, apart from the new cases.

      **Result.** 67 cases: 54 pass, 13 skipped (the twelve from before, and the reduced-
      configuration log case), 0 fail.

- [ ] 4.6 A scenario nothing here can exercise in reasonable time: *The file being downloaded
      is reused* needs a rotation, about 8.6 h of logging, during a download of the oldest file.
      Leave unticked unless someone runs it, and say so here.

## 5. Documentation and closing

- [x] 5.1 Update `ARCHITECTURE.md`: §4 (queue depths, `LinkMsg` size, the newlib allocation per
      download), §6 (`TaskSdWrite` answers the link; `TaskMavlink`'s new cases). Verify by
      re-reading it against the code.

      **Result.** The diagram's depths and stack, the task table, §4 (the `LinkMsg` size, both
      depths, and the newlib allocation the SD library makes on every open, which the section's
      "nothing allocates" claim had left out), §5.1's dispatch bullet, and a new part of §5.2 on
      reading the log from the ground.

- [x] 5.2 **[BOARD]** Read the high-water marks again after 4.2's downloads and compare with 2.1.
      Verify the differences are recorded here, and that any stack that had to grow is in the RAM
      figures of 2.1.

      **Result** (words free, after the HIL suite's downloads, against 2.1's baseline on
      `master`): `SdWrite` 87 of 256 -> 103 of 320 (217 used, as the spike predicted),
      `Mavlink` 129 -> 105, `UartWrite` 140 -> 113, `UsbWrite` 130 -> 112, the rest unchanged.
      No stack has to grow beyond the 320 words already counted in 2.1.

- [x] 5.3 Correct the proposal and design where the build or the spike proved them wrong, and
      state here which of the board steps ran and which did not.

      **Result.** Proposal and design now carry the measured RAM (headroom 1508 B, 1532 B spent)
      in place of the estimate, and the design says a listing or a new request briefly opens a
      second handle, so reading allocates at most two `SdFile`s at once, not one.

      **What ran on the board and what did not.** Ran: 1.1, 1.2, 1.3 (both halves, the second
      after `persist-the-log-ring-position`), 2.1, 3.3, 4.1, the active-file half of 4.2, 4.4,
      4.5, 5.2. Not run: 1.4 and 4.3, which need the card pulled and cannot be for now; the
      closed-file half of 4.2, because no closed file exists yet; 4.6, a rotation during a
      download of the oldest file; Download times over the UART were not measured: no adapter.
