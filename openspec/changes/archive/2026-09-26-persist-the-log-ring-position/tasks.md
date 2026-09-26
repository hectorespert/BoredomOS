# Tasks

**[BOARD]** needs the board on USB. `pio test -e libs` is **destructive**: it replaces the
firmware with the Unity binary and deletes `data*.BIN` and `index.bin` — the flight log — on
every case. Ask before running it, and follow it with `pio run -t upload`.

## 1. Test first, then the fix

- [x] 1.1 Add `test_sddata_resumes_the_file_it_was_writing_after_a_restart` to
      `test/test_libs/test_main.cpp` and its `RUN_TEST` line, as `design.md` describes. Verify
      `pio run -e libs` builds.

      **Result.** Written. `pio run -e libs` builds but does not compile `test/`, so the case
      was first compiled by 1.2's run.
- [x] 1.2 **[BOARD]** Run `pio test -e libs` against the unfixed library (after asking). Verify
      the new case fails, and on which assertion — the test has to fail for the reason this
      change exists before the fix can be said to pass it. Record the output here.
- [x] 1.3 In `lib/SdData/SdData.cpp`, open `index.bin` in `writeLogIndex()` with
      `O_WRITE | O_CREAT` instead of `FILE_WRITE`, and say why in a comment. Verify `pio run` on
      both environments and `pio check` with no new finding.

      **Result.** Both environments build; `RAM budget` unchanged (`headroom 3040 B`);
      `pio check` reports the same 2 LOW findings as before, none in `lib/SdData`.
- [x] 1.4 **[BOARD]** Run `pio test -e libs -v` again. Verify every case passes, the new one
      included, and record the counts. Then `pio run -t upload` to leave the board flying.

      **Result.** 21 cases, 21 pass. Flight firmware reflashed.

## 2. On the flight firmware

- [x] 2.1 **[BOARD]** Run the HIL suite (`pio test`, host kept awake). Verify it passes as
      before; this change touches no task body, so this is a regression check only.

      **Result.** After clearing the fault counters: 58 cases, 46 pass, 12 skipped (the
      adapter, reduced-configuration, RESET and restart cases), 0 fail.
- [ ] 2.2 **[HANDS]** When the card can be read — pulled, which is not possible at the time of
      writing, or downloaded once `download-the-flight-log` lands — let the flight firmware
      rotate at least once, restart it twice, and check that the file written before the
      restarts carries on growing and no other file changed. Leave unticked until then, and
      carry it into `TODO.md` at archive if it is still open.

## 3. Documentation

- [x] 3.1 Update `ARCHITECTURE.md` wherever it describes `index.bin` being written, if it says
      how. Verify by grep and re-reading.

      **Result.** §5 said the index is persisted but not how; one sentence added saying it is
      overwritten in place and why, since the paragraph was describing behaviour the firmware
      did not have.
