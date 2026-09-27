# Tasks

Legend: **[board]** needs the assembled board attached. **[hands]** also needs someone
physically at it. **[destructive]** is `pio test -e libs`, which replaces the firmware and
deletes the log on the card: ask before running it, and follow it with `pio run -t upload`.

## 1. Rotation order and the validated resume in `lib/SdData`

- [x] 1.1 In `SdData::write()`, move the removal of the next slot's file ahead of
  `writeLogIndex()`, so the order is close, remove, persist, open, preamble (design
  decision 1). Rewrite the comment above the rotation to state the invariant: the index
  never names a file from a previous lap. Verify: `pio run` builds, and the rotation block
  reads in that order.
- [x] 1.2 Factor the close / remove / persist / open / `notifyOpened()` sequence out of
  `write()` into one private method taking the slot to advance from, so `write()` and
  `begin()` share it rather than duplicate it. Verify: `pio run` builds, and `write()`
  calls it at the one rotation site.
- [x] 1.3 Rework `begin()` per design decision 2. Take the index (0 if missing or out of
  range) and resume there if the slot is absent or not full. Otherwise search the next
  `N-1` slots in ring order for one that is absent or not full, resume there and rewrite
  `index.bin`. If every slot is full, advance from the index through 1.2's method. "Full"
  uses the same `>= _size` comparison as `write()`. Hold at most one `File` open at a
  time. Verify: `pio run` builds, and `pio check` reports nothing new in `lib/SdData`.
- [x] 1.4 Update `SdData.h`: the comment on `begin()` (it no longer simply "opens the file
  the persisted index names") and the rotation description on `write()`. Verify: both
  describe the new order and the validation, and neither still says the index alone
  picks the file.
- [x] 1.5 Add Unity cases to `test/test_libs/test_main.cpp`, each building its card state
  by hand (a helper that writes a file of a given size, plus the existing
  `writeLogIndexByHand()`), then calling `begin()` and one `write()`, then `end()`:
  - *everything full, index `i`*: slot `i+1` is recreated and holds only the new
    bytes, and every other slot is unchanged (the "after close" cut);
  - *index `i` full, slot `i+1` absent*: resumes in `i+1`, and no other slot changes
    (the "after remove" cut);
  - *index missing, one partial slot that is not slot 0*: resumes in the partial slot,
    no other slot changes, and `index.bin` now names it;
  - *index names a full slot, a later slot is partial*: same result as the previous
    case.

  Register each in `runUnityTests()`. Verify: the cases compile under `pio run -e libs`.
- [x] 1.6 **[board] [destructive]** Run `pio test -e libs -v`. Verify: every case passes,
  the four new ones included (25 in total, from 21). Existing cases
  `test_sddata_begin_reopens_the_file_the_index_names` and
  `test_sddata_resumes_the_file_it_was_writing_after_a_restart` still pass unchanged:
  they are the "index is right" path. Then `pio run -t upload`.

## 2. Documentation and backlog

- [x] 2.1 Update `ARCHITECTURE.md`'s ring paragraph (§5.2, "`lib/SdData` is a
  fixed-footprint ring"). Give the new rotation order, the validation `begin()` does, and
  qualify "a file always starts empty, so it never holds records from an earlier lap"
  with the reason that is now true. Verify: nothing in the section still says the index
  is trusted unconditionally.
- [x] 2.2 Delete the `TODO.md` entry *Put the ring's position in the log data instead of
  `index.bin`* in the same commit that adds this change. Verify: `grep -n "ring's
  position in the log data" TODO.md` finds nothing, and no other entry referred to it
  (checked when this change was proposed: none did).

## 3. The flight firmware on the board

- [x] 3.1 `pio run`. Verify: it builds, and `scripts/ram_budget.py` reports headroom
  above the floor. Record the figure here. No change is expected: nothing was added to
  `.bss`. *Result: 1488 B headroom (floor 1024), 31280 B committed, identical to
  master.*
- [x] 3.2 **[board]** `pio run -t upload`, then `pio test`. Check `custom_mode` first, per
  `CLAUDE.md`, so the reduced configuration does not show up as a regression. Verify: the
  HIL suite shows no regression against the current master, in particular the
  log-listing and download cases.
  *Result: before the run `custom_mode` read ACTIVE, consecutive 1, cumulative 2. Of
  71 cases, 51 passed, 15 skipped themselves, and 4 failed:
  `test_sys_status_at_1hz`, `test_heartbeat_at_1hz`, `test_system_time_at_1hz` and
  `test_battery_status_every_2s`. All four failed on one 930 s sample window. `pmset -g
  log` shows the host asleep for 978 s from 20:45:57, and the run ended on SIGHUP. Rerun
  alone under `caffeinate -i` (`run.py --filter`), all four pass. A 10 s listen straight
  afterwards saw HEARTBEAT, SYSTEM_TIME and SYS_STATUS at 1 Hz and BATTERY_STATUS at
  0.5 Hz. The log-listing and download cases passed in the full run.*
- [x] 3.3 **[board]** Arm the `NAMED_VALUE_INT` housekeeping stream
  (`MAV_CMD_SET_MESSAGE_INTERVAL`, message id 252) after a boot, and read `SdWrite`'s
  stack high-water mark. Verify: record it here next to the value on current master. It
  must not approach zero, since `begin()` runs on that stack.
  *Result (words free, minimum-ever, after the full `pio test` above): `SdWrite` 90 of
  320, and `Mavlink` 94. The reference on master, in `report-the-faulting-task` 6.1 after
  a full `pio test`, was `SdWrite` 104 and `Mavlink` 104, and an older reading gave
  `SdWrite` 89. `Mavlink` fell by 10 with no code change, so run-to-run spread is about
  that size, and the 14 words here cannot be attributed to `begin()` with confidence.
  Either way 90 free is far from zero. The interval has a 1000 ms floor, and a
  200 ms request is denied.*
- [ ] 3.4 **[board] [hands]** The one real-card check nothing above does: with a card that
  has wrapped the ring, delete `index.bin` by hand (card pulled, or over FTP once that
  exists), restart the board, and confirm the log resumes in the file that was partial.
  Read the result off the log download. Verify: the partial file grew, and every other
  file's `LOG_ENTRY` size and `time_utc` are unchanged. A wrapped ring at the default size
  takes about 34 h of logging. If none is available, leave this unticked and say so. The
  Unity case in 1.5 covers the same logic at test sizes.
  *Not run: the card was just wiped by 1.6, so no wrapped ring exists. Carry this to
  `TODO.md` at archive time unless it has been done by then.*
- [ ] 3.5 Not reachable by any test: a power cut at a chosen point in rotation. The claim
  that decision 1 leaves only the four states 1.5 exercises rests on the SD library making
  `remove()` and the index write single directory updates. This row stays unticked, with
  this explanation, when the change is archived.
