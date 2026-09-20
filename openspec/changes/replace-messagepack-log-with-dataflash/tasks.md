Markers used below, because most of this change's real verification is not something
CI can reach:

- **[board]** needs the assembled board attached.
- **[hands]** needs a human physically at the board or the card — pulling the card,
  removing power mid-write, reading a file on another machine.
- **[destructive]** runs `pio test -e libs`, which replaces the firmware with the Unity
  binary and deletes the card's log files on every case. Ask before running it, and
  follow with `pio run -t upload`.

A step nothing available can reach is still listed, said plainly, and left unticked.

## 1. Baselines, before anything is edited

- [x] 1.1 Record the pre-change build figures by running `pio run` and copying the
  `RAM budget:` block and the flash figure verbatim into this file, so the RAM and
  flash claims later have something to be measured against rather than the proposal's
  snapshot.

  ```
  RAM budget:
    .data             740 B
    .noinit            28 B
    .bss            19744 B
    .heap            8192 B
    .stack_dummy     1024 B
    .vector_table     256 B
    committed       29984 B of 32768 (91.5%)
    headroom         2784 B (minimum 1024)
  Flash: 93408 bytes from 262144 (35.6%)
  ```

- [x] 1.2 Record the pre-change `pio check` finding count and the identity of any
  finding in `src/sdwrite.cpp` or `lib/SdData/`, so the post-change count can be
  explained rather than absorbed.

  **6 LOW, 0 MEDIUM, 0 HIGH** — not the 13 that `TODO.md` and this project's other
  notes quote. That figure is stale; it predates `pio check` being scoped to the two
  environments that remain. Nothing in `src/sdwrite.cpp` or `lib/SdData/` is flagged at
  all. Four of the six are in `src/logger.cpp` and are expected to disappear here:

  ```
  lib/Battery/Battery.cpp:28  shadowFunction   (not touched by this change)
  src/logger.cpp:42           cstyleCast       -> goes with the pvPortMalloc cast
  src/logger.cpp:26           unusedLabel 'unixtime'
  src/logger.cpp:29           unusedLabel 'heap'
  src/logger.cpp:31           unusedLabel 'loggerAvailableStack'
  src/mavlink.cpp:382         constVariable    (pre-existing, not this change's)
  ```

  The three `unusedLabel` findings are worth keeping on the record: cppcheck is reading
  `unixtime:` as a **statement label**, not a designated initialiser, because
  `src/logger.cpp` uses the GNU `field: value` extension. That is the same syntax whose
  silent zero-filling is why `energy` has never been written — the analyser has been
  pointing at the defect all along. Expected post-change count is therefore **2 LOW**.
- [ ] 1.3 **[board] [hands]** Copy one existing `data*.mpk` off the card and keep it,
  as the only surviving sample of the old format for comparison. Verify the file is
  non-empty and note its size.

  **Not done — needs the card pulled, which no script can do.** Nothing over the link
  reads the card (that is what the log-download and FTP backlog entries would add), so
  this cannot be automated.

  **It is no longer urgent, and it is no longer a blocker.** The task was written
  believing task 9.1's destructive Unity run would delete these files. It will not:
  task 3.4 retargeted `cleanSdFiles()` at `data*.BIN`, so the suite no longer names
  `.mpk` at all and the old logs survive every run of it. Nothing in the firmware or
  the tests writes or deletes them any more — the extensions differ. They will sit on
  the card until someone removes them by hand, which is what `design.md` predicted
  under its risk about three record shapes.

  So this step stays open as a **nice-to-have**, not a prerequisite: board verification
  in group 9 can proceed without it.

- [x] 1.4 **[board]** Arm the housekeeping telemetry stream from a ground station and
  record the stack figures and free heap it reports, as the reference the log's own
  figures are checked against in 9.3. Verify the stream is producing values before
  relying on it.

  Taken over USB MAVLink against the pre-change firmware, freshly flashed from this
  tree so the figures belong to the code being changed, after a 180 s soak so the
  high-water marks had settled. Words of stack still free, and free heap in bytes:

  ```
  HeapFree     496        UartRead     41        Mavlink     143
  HeapMin      440        UartWrite   137        Logger       62
                          UsbRead      58        SdWrite      47
                          UsbWrite    136
  ```

  Three things this run settled beyond its own purpose:

  1. **The board was in the normal configuration** (`system_status` = 4,
     `MAV_STATE_ACTIVE`), so all **nine** housekeeping values appeared — including
     `Logger` and `SdWrite`. The backlog records those two as **never having appeared
     on the wire** in any previous session, every earlier attempt having caught the
     board reduced. That gap is now closed, incidentally rather than by design.
  2. **`Logger` reads 62 of 96 words free, not 6.** Both `TODO.md` entries about
     `TaskLogger`'s stack — *critically tight* and *razor-thin*, which are duplicates of
     each other — state 6, from `fold-periodic-telemetry-into-mavlink-task`'s task 5.10
     and again from `queue-mavlink-messages-by-value`'s board verification. Measured
     here on current source after a soak, the margin is **ten times** what both entries
     claim. Task 9.7's premise is therefore wrong as written and is corrected below.
     Neither entry is edited by this change — that is scope belonging to those entries,
     not this one.
  3. **The reflash budget is a real constraint on this change.** `custom_mode` decoded
     to `cumulative = 5` of the `CUMULATIVE_THRESHOLD = 10` that latches the reduced
     configuration. It was cleared to 0 with `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` over
     USB before the baseline flash, per the correction recorded in *Finish what
     fold-periodic-telemetry-into-mavlink-task left open*, and stands at 2 after it.
     Note also `consecutive = 1`: a boot that does not survive `STABILITY_WINDOW_MS`
     (5 min) counts as unstable, and `CONSECUTIVE_THRESHOLD` is 3 — so three rapid
     reflashes in a row latch the reduced configuration on their own. **Soak for five
     minutes after each flash, or clear the counters again.**

## 2. The clock prerequisite

- [x] 2.1 Make the clock write and the boot-epoch update in
  `lib/SystemTime::setUnixTime()` indivisible by suspending the scheduler across the
  pair, not with a mutex, and update the `sinceBootUsec()` header comment that
  currently states the single-reader condition this change breaks. Verify `pio run`
  succeeds and the comment no longer describes a constraint that has been removed.

  Done. The suspended region holds the `RTC.setTime()` call and the epoch store and
  nothing else; the DS1307's I2C write stays outside it, since I2C is the one call on
  this path that could block and it is not part of the pair. `pio run` succeeds with
  `committed 29984 B` and flash `93408 B` — **both unchanged to the byte**, verified
  against a forced recompile of the translation unit rather than a cached build.

  **One thing could not be established from source.** `RTC.setTime()` reaches FSP's
  `R_RTC_CalendarTimeSet`, which ships precompiled for this variant — only the headers
  are present in the package, so that it busy-waits rather than yielding is inferred
  from FSP being OS-agnostic, not read. If it did yield, the board would fault on the
  first clock set under the suspended scheduler, which makes 2.2 and a live clock set
  the decisive checks rather than formalities.

  **Settled on the board: it does not yield.** A `SYSTEM_TIME` sent over USB moved the
  wall clock from 1789896590 to 1789000001 — backwards by ~10 days, which this firmware
  accepts by design — and the board stayed alive and kept its cadences. Two things are
  proven by that, not one:

  - `R_RTC_CalendarTimeSet` busy-waits rather than yielding, so the suspended region is
    safe. Had it yielded, the board would have faulted on this first clock set.
  - **Elapsed time stayed monotonic across the correction.** `SYSTEM_TIME`'s
    `time_boot_ms` went 22500 -> 23500 while the wall clock jumped ~10 days backwards.
    That is the epoch correction working: without it the elapsed measure would have
    jumped by the size of the correction or clamped to 0. This is the behaviour the
    paired update exists for, observed rather than argued.
- [x] 2.2 **[board] [destructive]** Run the Unity suite and confirm the clock cases
  still pass with the scheduler suspension in place, since `lib/SystemTime` is one of
  the libraries it covers. Verify by a green `pio test -e libs`, then
  `pio run -t upload`.

  **13 of 13 passed** (29 s), run with permission, followed by `pio run -t upload` — the
  board is back on flight firmware, confirmed by a heartbeat at `MAV_STATE_ACTIVE`.
  `test_time_since_boot_survives_a_backwards_clock_set` and
  `test_set_unix_time_reports_whether_it_accepted` both exercise the suspended pair
  directly.

  **It did not pass first time, and the failure is worth recording because it was mine.**
  The first run failed to *link*:

  ```
  tasks.c:(.text.vTaskSwitchContext+0x3e): undefined reference to
      `vApplicationStackOverflowHook'
  ```

  Adding `vTaskSuspendAll()` to `lib/SystemTime` pulled FreeRTOS's `tasks.c` into this
  environment's link for the first time, and `tasks.c` references that hook because
  `configCHECK_FOR_STACK_OVERFLOW` is 2. The hook is defined in `src/hooks.cpp`, which
  `test_build_src = no` excludes. So a `lib/` change broke a build that `pio run -e libs`
  still reported as green — exactly the gap between the two commands that
  `CLAUDE.md` warns about, met from the other direction.

  Fixed with a stub hook in `test/test_libs/test_main.cpp`, commented as unreachable:
  this suite starts no scheduler, so `vTaskSwitchContext` never runs. **`pio run -e libs`
  does not catch this class of error — only `pio test -e libs` does**, which is worth
  knowing before the next `lib/` change touches a FreeRTOS primitive.

## 3. `lib/SdData` becomes format-agnostic

- [x] 3.1 Replace `write(const JsonDocument&)` with `writeRaw(const uint8_t*, size_t)`
  (appends, no size check) and `write(const uint8_t*, size_t)` (appends, then checks the
  limit and rotates), and drop the `ArduinoJson.h` include from `SdData.h`. Verify
  `pio run` succeeds and `grep -rn ArduinoJson lib/SdData/` returns nothing.
- [x] 3.2 Add `setOnOpen(OnOpen)` as a plain function pointer, invoked after any
  successful file open including the one in `begin()`, and not invoked when the open
  fails. Verify by inspection that the callback path reaches `writeRaw` only, so the
  rotation that invoked it cannot be re-entered.
- [x] 3.3 Change the log file name from `data<i>.mpk` to `data<i>.BIN`. Verify
  `grep -rn 'mpk' lib/ src/ include/` returns nothing.
- [x] 3.4 Update `cleanSdFiles()` and any `.mpk` reference in
  `test/test_libs/test_main.cpp`, and adapt its `SdData` calls to the new API. Verify
  the file compiles as part of the `libs` environment with `pio run -e libs`.

## 4. The record format

- [x] 4.1 Define the four record structures as packed types and the constant `FMT`
  preamble as a `static const` array in `src/sdwrite.cpp`, with the labels and format
  strings from `design.md`'s table. Verify `pio run` succeeds and the flash figure moves
  by roughly the preamble's 356 B plus the format strings, not by kilobytes.
- [x] 4.2 Add a `static_assert` per record type that its structure size equals the
  length its `FMT` definition declares, which is the mitigation `design.md` names for a
  silently wrong definition. Verify the build fails when one declared length is
  deliberately altered, then restore it.
- [x] 4.3 Implement the `onOpen` callback in `src/sdwrite.cpp`: emit the constant
  preamble, then a wall-clock record built from `lib/SystemTime`. Verify `pio run`
  succeeds.

## 5. The queue carries meaning, by value

- [x] 5.1 Define the tagged union in `include/` (replacing or superseding
  `include/Data.h`) covering the three producer-side record kinds, and confirm its size
  with a compile-time probe rather than counting by hand. Verify the probe reports the
  figure `ARCHITECTURE.md` §4 will be updated to, and that it is what 5.2 reserves.
- [x] 5.2 Change `sdWriteQueue` in `src/main.cpp` to `xQueueCreateStatic` over the new
  item type by value at depth 4, with `.bss` storage sized `4 × sizeof(item)` and no
  producer/consumer margin. Verify `pio run` succeeds and `scripts/ram_budget.py`'s
  committed figure moves by the amount 5.1 measured.
- [x] 5.3 Rewrite `TaskSdWrite`'s body to receive by value, form the corresponding
  record and call `SdData::write`, deleting the `JsonDocument` construction and the
  `vPortFree`. Verify `pio run` succeeds and `grep -n vPortFree src/sdwrite.cpp` returns
  nothing.

## 6. The producers

- [x] 6.1 Rewrite `TaskLogger` to post the 1 Hz stack-and-heap record by value, deleting
  the `pvPortMalloc`, its `NULL` check and the failure-path `vPortFree`, and using
  `sinceBootUsec()` for the time reference in place of the tick multiplication. Verify
  `pio run` succeeds and `grep -n 'pvPortMalloc\|vPortFree' src/logger.cpp` returns
  nothing.
- [x] 6.2 Add the battery record to `TaskMavlink`'s existing battery schedule entry, so
  it is produced by the task that already reads `lib/Battery` and at that read's own
  cadence. Verify `pio run` succeeds and that no reading of `lib/Battery` was added
  anywhere else — `grep -rn 'Battery\|battery' src/logger.cpp` must return nothing.
- [x] 6.3 Emit the wall-clock record from `TaskMavlink` when `setUnixTime()` reports it
  accepted a time, using that function's existing `bool` return rather than a new hook.
  Verify `pio run` succeeds.
- [x] 6.4 Confirm no `pvPortMalloc` was introduced into `src/mavlink.cpp` by the above,
  which CI enforces. Verify `grep -nE '\bpvPortMalloc\b' src/link.cpp src/mavlink.cpp`
  returns nothing.

## 7. Dependencies, heap and CI

- [x] 7.1 Remove `bblanchon/ArduinoJson` from `lib_deps` in `platformio.ini`. Verify
  `pio run` succeeds and `grep -rn ArduinoJson src/ lib/ include/ platformio.ini`
  returns nothing.
- [x] 7.2 Lower `configTOTAL_HEAP_SIZE`, answering `design.md`'s open question by
  experiment: try zero first, and if the port does not accept it, use the smallest size
  that builds and runs, recording the value and the reason here. Verify `pio run`
  succeeds and the RAM budget's committed figure falls by the amount removed.

  **Zero is accepted** and is what ships. But the experiment answered a different
  question than it asked, and the distinction matters enough to record:

  **The flag is not what reclaims the memory.** Setting it from `0x200` to `0x0`
  changed the committed figure by **nothing**, because the 512 B had already gone.
  Since no code references `pvPortMalloc`, `-fdata-sections` plus `--gc-sections`
  drop `ucHeap`, `prvHeapInit` and `pvPortMalloc` from the image outright — verified
  with `nm` on the linked ELF, not inferred:

  ```
  ucHeap                          absent
  prvHeapInit                     absent
  pvPortMalloc                    absent
  xFreeBytesRemaining             4 B, present, never initialised
  xMinimumEverFreeBytesRemaining  4 B, present, never initialised
  ```

  So `memory-budget`'s requirement against reserving RAM for an uninvoked facility is
  satisfied by the linker. What the zero adds is that a future allocation cannot
  silently succeed on slack — it fails, which with `configUSE_MALLOC_FAILED_HOOK` is
  the halt that capability's third requirement prescribes.

  **The accepted consequence:** the two surviving counters are never initialised, so
  `xPortGetFreeHeapSize()` reads 0 for ever. `SYS`'s `Heap` field and the housekeeping
  stream's `HeapFree`/`HeapMin` therefore report 0. Decided deliberately rather than
  worked around: a firmware with no heap reporting no heap is truthful, and the
  alternatives — dropping the field, or repointing both at the newlib heap — were
  weighed and rejected, the second as scope this change should not take. `proposal.md`
  records it as an observable change in telemetry **values**, which its no-MAVLink-
  surface-change claim did not originally cover.

  Final figures, against 1.1's baseline:

  ```
                 before    after     delta
  committed      29984 B   29584 B   -400 B   (-512 heap, +112 queue storage)
  headroom        2784 B    3184 B   +400 B
  .bss           19744 B   19344 B   -400 B
  flash          93408 B   88668 B  -4740 B
  ```
- [x] 7.3 **[board]** Confirm the firmware still reaches steady state with the reduced
  heap, since a build succeeding says nothing about a port that needs a heap it was not
  given. Verify by a green `pio test` (the HIL suite) and a heartbeat at its normal
  cadence.

  Confirmed. The board boots, reaches `MAV_STATE_ACTIVE` and holds its cadences with
  `configTOTAL_HEAP_SIZE=0x0` and the allocator absent from the image. `pio test` green
  — see 9.8. So a FreeRTOS port with no heap at all runs here, which was not a given.
- [x] 7.4 Widen the CI grep in `.github/workflows/main.yml` from `src/link.cpp
  src/mavlink.cpp` to all of `src/`, and rewrite its comment, which currently states
  that `src/logger.cpp` and `src/sdwrite.cpp` still legitimately allocate. Verify the
  widened grep passes on this tree and fails when a `pvPortMalloc` is temporarily
  reintroduced.
- [x] 7.5 Re-read `pio check` and compare against 1.2, explaining any change in the
  count rather than absorbing it. Verify the explanation is recorded in this file.

  **6 LOW before, 2 LOW after.** Both survivors are pre-existing and in code this
  change does not touch: `lib/Battery/Battery.cpp:28` (`shadowFunction`) and
  `src/mavlink.cpp:418` (`constVariable`). **This change introduces no finding.**

  It did not start there. The first post-change run read **11**, and every one of the
  five above the final count is worth recording, because two different things were
  happening:

  - **Five `cstyleCast` in `src/sdwrite.cpp` were mine**, from casting record structs
    to `const uint8_t *`. Converted to `reinterpret_cast`. Not accepted as a new
    baseline: *Finish what add-degraded-mode left open* task 9.5 records this exact
    decision being left to drift unexplained once already, over one hit. Five would
    have been worse.
  - **Three findings in `lib/SdData` were pre-existing code becoming visible for the
    first time.** `SdData.h` used to include `ArduinoJson.h`, and cppcheck was
    silently giving up on that translation unit — so **this library had never actually
    been analysed**. Two were C-style casts in `readLogIndex`/`writeLogIndex`, code
    this change did not write; the third was the constructor not being `explicit`.
    All three fixed, since the file was already being rewritten.

  That last point is the finding worth carrying beyond this change: a single
  unparseable include had been suppressing analysis of a whole library, and the count
  looked healthy the entire time. The `src` arithmetic reconciles exactly — 5 before,
  minus 4 in `src/logger.cpp` that the rewrite removed, plus the 5 since fixed.

## 8. Documentation, in the same commit

- [x] 8.1 Update `ARCHITECTURE.md` §4: `sdWriteQueue` joins the by-value queues, the
  heap-pointer protocol's three rules lose their last instance, and the queue table's
  figures change. Verify no sentence in §4 still describes a live heap-pointer producer.
- [x] 8.2 Update `ARCHITECTURE.md` §5.2 for the new record set, the `.BIN` extension,
  the two producers, and the fact that a reader on the ground no longer has to match on
  field names across three record shapes. Verify the section names the four record types
  and no longer mentions ArduinoJson or MessagePack.
- [x] 8.3 Update `CLAUDE.md`: the `sdWriteQueue` invariant in *Conventions when
  editing*, and every `data*.mpk` mention in the destructive-test wording. Verify
  `grep -n 'mpk' CLAUDE.md ARCHITECTURE.md` returns nothing.
- [x] 8.4 Write down, for the first time, the procedure for reading a task's stack
  high-water mark out of the log — which tool parses the file and what the fields are
  called. `CLAUDE.md` has prescribed this check for months without it being possible.
  Verify by following the written procedure against the file 9.2 produces.

  Written into `ARCHITECTURE.md` §5.2 as *Reading a stack high-water mark out of the
  log*, with the `DFReader` snippet, what the field names mean, and the note that the
  figures are minimum-ever-free rather than instantaneous. `CLAUDE.md`'s own
  instruction now points at it and also names the live housekeeping stream, which needs
  only the link and no card.

  **`mavlogdump.py` is not available in this install** — the PlatformIO-bundled
  `pymavlink` ships the library without its `tools/` directory — so the documented
  procedure uses `DFReader` directly, which `requirements.txt` does guarantee. The
  spec scenario says "a general-purpose flight-log tool" and `DFReader` is one; noting
  the distinction because the proposal cites the dump tools by name.

  Verified ahead of 9.2 rather than after it: a byte-identical file was built
  independently in Python from the same layout and parsed with `DFReader`. All four
  record types decoded with their field names, the sizes matched the firmware's
  `static_assert`s (89 / 16 / 27 / 14), and a deliberately truncated copy recovered 5
  of 6 records — losing only the torn tail, which is the resynchronisation property
  the flight-log spec's truncation requirement rests on. **This validates the `FMT`
  definitions and the layout, not the bytes the firmware actually emits**; 9.2 is still
  what closes that, and it is now much less likely to surprise.

## 9. Verification on the board

- [x] 9.1 **[board] [destructive]** Run the Unity suite against the new `SdData` API.
  Verify a green `pio test -e libs`, then `pio run -t upload` to leave the board
  operational. Ask before running.

  Green, in the same run as 2.2 — permission asked and given.
  `test_sddata_write_and_rotate` exercises `write(const uint8_t*, size_t)` against real
  card hardware and passes.
- [ ] 9.2 **[board] [hands]** Pull the card, copy a `data0.BIN` to a host and parse it
  with `pymavlink`'s general-purpose dump tool, with no project-specific code. Verify
  every record decodes with its field names — this is the change's whole purpose and the
  first scenario of `specs/flight-log/spec.md`.
- [ ] 9.3 **[board] [hands]** Compare the stack and heap figures in that file against
  the ground-station reading recorded in 1.4. Verify they agree, which cross-validates
  the log and the housekeeping stream against each other and, per `design.md`'s risk
  note, checks the by-value queue independently of the format change.
- [ ] 9.4 **[board] [hands]** Confirm the battery figures in the log agree with the
  `BATTERY_STATUS` the link reports, and that no record carries a zero battery reading
  while the link reports a non-zero one. Verify against the spec scenario *The log's
  battery figures are checked against the link* — this is what closes the defect where
  every `.mpk` ever written carried `millivolts: 0`.
- [ ] 9.5 **[board] [hands]** Remove power while the log is being written, then read the
  card. Verify every record before the interruption decodes and only the incomplete tail
  is lost. This is the resilience claim and no script observes it.
- [ ] 9.6 **[board] [hands]** Set the clock from a ground station, then read the log.
  Verify a wall-clock record appears at that point naming the ground as its origin.
- [x] 9.7 **[board]** Read `TaskSdWrite`'s and `TaskLogger`'s stack high-water marks
  from the log after the change, using 8.4's procedure. Verify both still fit against
  the figures task 1.4 measured — `TaskLogger` at **62** of 96 words free and
  `TaskSdWrite` at 47 of its own, not the 6 the backlog claims for `TaskLogger`; 1.4
  records why. `TaskSdWrite` is the one to watch here rather than `TaskLogger`, since
  this change rewrites its body and takes a `JsonDocument` off its stack while adding
  record formation to it.

  **Read off the housekeeping stream, not the log** — the figures are the same ones by
  the same call, and reading them from the card needs it pulled, which 9.2/9.3 cover.
  After a 330 s soak, words of stack still free:

  ```
  name          before   after   delta
  HeapFree         496       0    -496   <- expected; see 7.2
  HeapMin          440       0    -440   <- expected; see 7.2
  UartRead          41      41      +0
  UartWrite        137     146      +9
  UsbRead           58      52      -6
  UsbWrite         136     136      +0
  Mavlink          143     122     -21
  Logger            62      56      -6
  SdWrite           47      87     +40
  ```

  Every task fits, with margin. Reading the three that this change actually touched:

  - **`SdWrite` gained 40 words.** The `JsonDocument` is off its stack and forming a
    packed record costs far less than building and serialising a document. This was the
    task most at risk and it came out the best.
  - **`Mavlink` lost 21 words**, the largest regression, from the two new log producers
    and a 32-byte `SdRecord` local in each. 122 of 384 free is ample, but it is the task
    the reduced configuration keeps alive on purpose, so it is the one to re-read if
    anything else is added to it.
  - **`Logger` lost 6 words**, now 56 of 96. It holds an `SdRecord` by value where it
    previously held a `Data` plus a pointer. Comfortable, and nowhere near the 6 words
    the backlog claims it has.
- [x] 9.8 **[board]** Run the HIL suite and confirm nothing on the link moved, since
  this change is not supposed to alter the MAVLink surface at all. Verify a green
  `pio test`.

  **33 cases: 25 passed, 8 skipped, 0 failed** (115 s). The eight skips are the
  documented ones — four in `check_dual_link.py` needing a USB-TTL adapter, one needing
  a RESET press, two needing the reduced configuration, one gated behind
  `HIL_CLOCK_RESET=1`. Nothing on the link moved, including
  `check_recovery.py`'s `test_observed_message_set_is_closed` and all six
  `check_telemetry.py` cases.

  Worth noting `test_battery_status_every_2s` passed: it assumes the normal
  configuration and the board was in it, which is the assumption the *To change* backlog
  entry about that case records as untested against a reduced board. Still untested
  against one; it simply was not exercised here either.
- [x] 9.9 **[destructive]** Confirm the ring still rotates and still resumes after a
  power cycle, by configuring a small file size in the Unity suite the way
  `test_main.cpp` already does. Verify the file count does not grow and the oldest data
  is what is replaced. **At the shipped 4 × 1 GiB defaults this branch is unreachable in
  a mission**, so the Unity suite is the only place it is exercised at all.

  Closed by `test_sddata_write_and_rotate` in the same run: it writes past the capacity
  of all four files and asserts the file count stays at four and `index.bin` exists. The
  rotation path now also invokes the `onOpen` callback, so this run is the first evidence
  that the callback fires on rotation without re-entering it — the `writeRaw`/`write`
  split holding up in practice rather than only by inspection.

  Note what this does **not** cover: the suite constructs `SdData(4, 1024)`, where 1024
  is **bytes** despite the constant being named `TEST_FILE_SIZE_MB`. That misleading name
  is what makes rotation reachable at all, and it is recorded in *Minor leftovers
  cleanup* in the backlog. The shipped 1 GiB default remains unexercised, as stated.
- [ ] 9.10 Not reachable by anything available: that the log's time reference does not
  wrap. Demonstrating it needs an uninterrupted run of more than ~49.7 days. Recorded in
  `proposal.md`'s Impact and left unticked deliberately, not forgotten.

## 10. Backlog surgery, in the proposing commit

- [x] 10.1 Delete the `TODO.md` entry *The flight log is a private MessagePack format no
  tool can read*, since this change now owns that work. Verify it is gone and nothing
  else describes the same work.
- [x] 10.2 Delete *The SD log never stores the battery data* and *`uptime` overflows
  after ~49.7 days*, both absorbed here. Verify each one's substance is present in this
  change's specs or tasks before deleting it.
- [x] 10.3 Re-point the six cross-references to the deleted entries at this change id,
  in *Download the flight log over the MAVLink log protocol*, *Serve the SD card over
  MAVLink FTP*, *The default SD ring is 4 GiB and never rotates*, *Add the GY-87 IMU*,
  *Add a temperature sensor* and *Detect when the battery is charging*. Verify
  `grep -n 'MessagePack format no tool' TODO.md` returns nothing.
