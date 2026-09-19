Markers: **[board]** needs the assembled board attached · **[hands]** needs someone
physically at the board (disconnecting the DS1307, pressing RESET) · **[destructive]**
replaces the firmware and/or wipes the card, and must be followed by
`pio run -t upload` · unmarked tasks are closable from a checkout and CI alone.

## 1. Measure before building

These come first because two design decisions rest on them, and a negative result is
cheaper here than after the code exists (design.md — Migration Plan).

- [x] 1.1 Record the current RAM commitment and headroom from a fresh
  `pio run -e uno_r4_minima`, reading `scripts/ram_budget.py`'s printed total rather than
  the `RAM:` line, and note `sizeof(LinkMsg)` as it stands. This is the baseline every
  later RAM claim is measured against.
- [ ] 1.2 **[board]** **NEEDS A USB-TTL ADAPTER ON D0/D1** (`HIL_UART_PORT`). Determine
  whether the internal RTC keeps running across a software reset, observed through the
  found-running fact of task 3.5 — the DS1307 cannot be disconnected on this board, so the
  ladder would otherwise hide it. Attempted over USB and it **cannot** be closed there: the
  CDC port drops on reset (measured: the host handle dies with `Errno 6` within 100 ms of
  the reboot ack) and the boot report is emitted into a port with no host attached while it
  re-enumerates. Over the UART the adapter stays enumerated and the window does not exist.
  The HIL case is written and gated behind `HIL_CLOCK_RESET=1`
  (`check_clock.py::test_the_clock_survives_a_commanded_restart`); run it with `HIL_PORT`
  pointed at an adapter to close this. Recorded as a backlog entry of its own — *The boot
  `STATUSTEXT` cannot be observed over USB after a reset* — since it affects the existing
  boot text too, not just this change. **Consequence: 2.6's `survived` branch ships
  unexecuted.** Left unticked deliberately.
- [ ] 1.3 **[board]** **[destructive]** Measure how far the `LOCO`-driven internal RTC
  drifts from the DS1307. No path on the link exposes the two clocks separately, so this
  is a Unity case that reads both and prints the difference — write it alongside 2.7 and
  run `pio test -e libs` twice, at least an hour apart, with `pio run -t upload` after
  each. Verified by a recorded figure in s/hour; it sets the re-seed interval in 3.6 and
  answers design.md's Open Question. **Ask before each run**: each one wipes
  `data*.mpk` and `index.bin`.
- [x] 1.4 Establish whether a `platformio.ini` `-D RTC_CLOCK_SOURCE=...` reaches the
  core's `RTC.cpp` compilation unit at all, by defining it to the value it already has
  and checking the flag appears in the compile command for that unit
  (`pio run -v`). Record the answer and **do not change the clock source** — per
  design.md this change measures, a follow-up acts. Verified by the recorded command line.

### Measured (session of 2026-09-19, before any code was written)

- **1.1 baseline**, fresh link of `uno_r4_minima`: `.data` 740 B, `.noinit` 28 B,
  `.bss` 19612 B, `.heap` 8192 B, `.stack_dummy` 1024 B, `.vector_table` 256 B —
  **committed 29852 B of 32768 (91.1 %), headroom 2916 B** against a floor of 1024.
  Matches `ARCHITECTURE.md` §7.
- **`sizeof(LinkMsg)` is 64 B**, exactly the cap its `static_assert` names. Measured
  with the target toolchain, and measured again against a copy of the header carrying
  the `int64_t tc1` of task 3.1: **still 64 B**, because the union is sized by the
  52-byte `statustext` variant. Task 3.1's RAM claim is therefore already evidenced —
  it still needs confirming from a real link once the field is actually added.
- **1.4: project `build_flags` do reach `RTC.cpp`.** Its compile line carries
  `-DWDT_TIMEOUT_MS=1398`, `-DconfigTOTAL_HEAP_SIZE=0x200` and
  `-DMAVLINK_COMM_NUM_BUFFERS=2`, so a `-D RTC_CLOCK_SOURCE=...` would be honoured by
  the core's `#ifndef`. The other half of the sub-clock question — whether the Minima
  populates the 32.768 kHz crystal — is a schematic fact not answerable from the
  checkout, and remains open for 6.3.
- **`time_t` on this toolchain is 8 bytes, aligned to 8** — not 4, as design.md assumed.
  See the blocker recorded below.

## 2. `lib/SystemTime`: the clock itself

- [x] 2.1 Start the internal RTC unconditionally, replacing
  `if (_ds1307.begin() && RTC.begin())`, and skip every DS1307 write when the DS1307 did
  not answer (`add-degraded-mode` tasks 6.1). Verified by `pio run` plus 2.7's Unity
  cases; the no-DS1307 half is closed by 5.4. **Ticked for the DS1307-present half only:
  the guard that skips the DS1307 write when it is absent is written but unexercised,
  because 5.4 cannot run on this board.**
- [x] 2.2 Store the boot epoch as `uint32_t` — `time_t` is 8 bytes on this toolchain, so
  it would not give the single-aligned-word store the design depends on — and assert both
  that the stored member is 4 bytes and that the plausibility floor fits in it. Verified
  by `pio run` succeeding and by temporarily inverting each assertion to confirm it fires.
- [x] 2.3 Add the plausibility predicate — a fixed `2022-01-01T00:00:00Z` floor — and
  expose it, since both the inbound validation and the `survived` detection use it.
  Verified by the Unity case in 2.7 covering the floor, one second either side of it, and
  zero.
- [x] 2.4 Give `getUnixTimeUsec()` and `getUnixTimeNsec()` the `R64CNT` fraction with the
  seconds/`R64CNT`/seconds retry described in design.md. Verified by the Unity case in
  2.7 that hammers the accessor and asserts the value never decreases.
- [x] 2.5 Latch `_bootEpoch` at the end of `begin()`, after the origin has been selected,
  and add the elapsed-time accessor in both µs and ns. Put the concurrency obligation in
  the header beside it: single reader today, and the pair update must become indivisible
  with the scheduler suspended before a second task reads it. Verified by the Unity case
  in 2.7 asserting elapsed time advances and starts near zero.
- [x] 2.6 Add the origin (ground / ds1307 / survived / none), derived from
  `RTC.isRunning()` plus a `getTime()` that passes 2.3's floor — never from
  `RTC.begin()`'s return value, which is `true` on failure too. Rewrite `setUnixTime()`
  to return whether it accepted, validate against the floor, reject a lower-ranked
  origin, apply the delta to both the clock and `_bootEpoch`, and write the DS1307
  whenever it is present instead of returning early when the internal clock agrees.
  Verified by 2.7's cases for acceptance, rejection, the epoch shift and the DS1307
  write-through.
- [x] 2.7 **[board]** **[destructive]** Extend `test/test_libs/test_main.cpp` with the
  cases 2.2–2.6 name — sub-second monotonicity under hammering, the floor's boundaries,
  `setUnixTime()`'s return value and rejection, elapsed time staying continuous across a
  backwards clock set, and the DS1307 holding what the ground set. Run `pio test -e libs`
  (wipes `data*.mpk` and `index.bin`, leaves the Unity binary on the board), then
  `pio run -t upload`. Verified by the suite passing.

## 3. `src/mavlink.cpp` and `include/LinkMsg.h`

- [x] 3.1 Add `int64_t tc1` to `LinkMsg`'s `timesync` variant and confirm from a build
  that `sizeof(LinkMsg)` and both link queues' storage arrays are unchanged against
  1.1's baseline, and that `static_assert(sizeof(LinkMsg) <= 64)` still holds. Verified
  by the fresh `ram_budget.py` figure matching 1.1.
- [x] 3.2 Capture elapsed time into `intent.timesync.tc1` where `TaskMavlink` handles the
  inbound `TIMESYNC`, and make `mavlinkPack()` copy it instead of calling
  `systemTime.getUnixTimeNsec()`. Verified by 5.3.
- [x] 3.3 Validate inbound `SYSTEM_TIME` through `setUnixTime()`'s return value and emit
  nothing when it is rejected. Verified by 5.2 and 5.6.
- [ ] 3.4 Emit `SYSTEM_TIME.time_unix_usec` as `0` while the origin is `none`, leaving
  `time_boot_ms` as it is. Verified by 5.4.
- [ ] 3.5 Add two facts to the boot `STATUSTEXT` beside the reset reason and boot phase it
  already carries: the origin, and — separately, not derived from it — whether a clock was
  already running with a plausible time when `begin()` started. Emit one `STATUSTEXT` when
  the origin changes later. The second fact is what makes 1.2 and 5.5 observable at all on
  a board whose DS1307 is always attached, since `ds1307` outranks `survived` and would
  otherwise hide it. Check the text still fits `LinkMsg`'s 50-byte `statustext` field
  without truncation. Verified by 5.2 reading the text on a clock set, and by 5.5.
- [ ] 3.6 Add the DS1307 re-seed to `TaskMavlink`'s schedule at the interval 1.3
  supports, active only while the origin is `ds1307` or `survived`, emitting nothing.
  Verified by 5.7's high-water-mark check for the task's stack and by confirming from
  1.3's figure that the interval is shorter than the drift budget it was chosen for.

## 4. `src/main.cpp`

- [x] 4.1 Replace `systemTimeAvailable` — written once, read nowhere — with the origin
  from 2.6, and keep reporting the clock's absence at boot as
  `specs/fault-recovery/spec.md` requires. Verified by a grep showing no remaining
  reference to the old flag, and by 5.4. **Ticked for the grep half**: the only remaining
  occurrence is the comment recording why the flag is gone. Reporting the absence of a
  clock is implemented — the boot report names origin `none` — but unexercised, since 5.4
  cannot run on this board.

## 5. Verify from the ground

- [x] 5.1 **[board]** Run the HIL suite unchanged first (`pio test`) to confirm nothing
  in sections 2–4 regressed the existing 25 cases before new ones are added.
- [x] 5.2 **[board]** Extend `test/test_hil/check_clock.py`: the sub-second part of
  `SYSTEM_TIME` is not always zero and never decreases across a sample; an implausible
  time (zero, and a pre-2022 value) leaves the reported time where it was; a plausible
  one is adopted. Verified by the cases passing via `run.py --filter`.
- [x] 5.3 **[board]** Extend `test/test_hil/check_timesync.py` past today's `tc1 > 0`:
  `tc1` is elapsed time since boot, not a UNIX timestamp — orders of magnitude smaller
  than the host's own clock in ns — and two requests separated by a known wait differ by
  roughly that wait. Verified by the cases passing.
- [ ] 5.4 **[board]** **[hands]** **CANNOT BE CLOSED ON THIS BOARD.** With the DS1307
  disconnected: `SYSTEM_TIME.time_unix_usec` is `0`, `time_boot_ms` advances, the boot
  report names the origin as none, and a plausible time from the ground is then adopted.
  The DS1307 is not disconnectable on this assembly, so no observer can run this — it is
  `add-degraded-mode` task 6.4, blocked for the third time, and the `system-clock`
  requirement it would close ships unexercised and disclosed in proposal.md — Impact.
  Left unticked deliberately; do not tick it on the strength of a build or a reading of
  the code.
- [ ] 5.5 **[board]** **NEEDS A USB-TTL ADAPTER ON D0/D1.** Set a plausible time, command a
  restart with `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, and confirm the next boot's report
  states a clock was found already running with a plausible time, and that the time
  reported afterwards is not zero and no earlier than what was set. Closes the
  `system-clock` requirement "A clock set from the ground outlives a reset". The case is
  written and passes its setup over USB — the reboot is commanded and acknowledged — but
  fails at the observation for the reason recorded in 1.2. Same observation as 1.2: run it
  once over the UART and record it against both. Left unticked deliberately.
- [x] 5.6 **[board]** Send `SYSTEM_TIME` carrying an implausible time as fast as the link
  allows and confirm `HEARTBEAT`, `SYSTEM_TIME` and `BATTERY_STATUS` hold their rates and
  that nothing is emitted in response. Covers the `mavlink-link` scenario this change
  adds.
- [x] 5.7 **[board]** Read the stack high-water marks out of the SD log after the above,
  with attention to `TaskMavlink` — whose body grew — and confirm each task still has
  margin. Required by `CLAUDE.md` for any change to a task body; a green build says
  nothing about it.
- [x] 5.8 Run `pio check` and confirm the finding count against the current baseline,
  accounting for any new C-style cast in the same way task 9.5 of the archived
  `add-degraded-mode` entry still leaves open. Verified by the reported count.

### Measured on the board, with the change in place

- **RAM**: committed 29984 B of 32768 (91.5 %), **headroom 2784 B** against the 2916 B
  baseline. 4 of those bytes are the clock's own state; the other 128 are the two per-port
  write queues going from depth 5 to 6, which Copilot's review showed was needed — the
  figure matches that derivation exactly, and the floor is 1024. `sizeof(LinkMsg)` is unchanged at 64 B, which the 4-byte delta confirms
  on its own: had the union grown, the two link queues' storage would have cost 80 B more.
- **HIL**: 33 cases, 25 passed, 8 skipped (four need an adapter, two need the reduced
  configuration set up by hand, one needs a RESET press, one is the gated restart case).
  All 25 that ran include the seven added here. No pre-existing case regressed.
- **Stack high-water marks** on the delivered firmware, free words of allocated:
  `Mavlink` **136 of 384** (the task whose body grew), `UartWrite` 137/384,
  `UsbWrite` 136/384, `Logger` 68/160, `UsbRead` 58/128, `SdWrite` 53/256,
  `UartRead` 41/96. Heap free 496 B, minimum ever 440 B
  of the 512 B `configTOTAL_HEAP_SIZE`. `Mavlink` keeps 38 % of its stack; the tightest is
  `SdWrite`, which this change does not touch.
- **Unity**: 13 cases, all passing, including the eight added here, two of them reporting
  cases rather than assertions about behaviour.
- **`R64CNT` is a 7-bit counter at 128 Hz**, measured rather than deduced:
  `lowest=0 highest=127 transitions=260` over two seconds. The first implementation here
  masked six bits and scaled by 15625 µs, taking the register's "64-Hz Counter" name at
  face value; the fraction then wrapped twice a second and the reported time fell by
  almost a full second halfway through every second. **The Unity hammer case caught it on
  its first run; the 1 Hz HIL sampler could not have** — at one-second spacing the whole
  second always dominates a sub-second regression, so that case passes either way. Fixed
  to `& 0x7F` and `* 15625 / 2`, and `test_report_r64cnt_range` now pins the register's
  behaviour so the next reader does not have to trust the reasoning.
- **1.3, first of two readings**: `internal=1789842571 ds1307=1789842571 difference=0 s`.
  Zero is expected at this point — `begin()` had just seeded the internal clock from the
  DS1307 — so this reading is the t0 anchor, not the drift. The second reading, at least
  an hour later, is what produces a figure.
- **Copilot's review**: 7 findings plus several in its per-file table, all addressed — two
  were regressions this change had introduced (every accepted `SYSTEM_TIME` rewriting the
  DS1307 over I2C, and the write queues being one item too shallow for the new status
  text). One overstated its impact: it read the epoch-zero guard as stalling elapsed time
  permanently in the no-clock configuration, where the stall lasted one second; the guard
  was wrong anyway, since 0 was overloaded as both a failed read and a valid reading, and
  is now a `bool` from the read itself. A second pass raised four more, all taken: the
  reference GCS sends `SYSTEM_TIME` once a second, so repeating the second the clock
  already holds is now a no-op and reconciling a drifted DS1307 moved to the periodic path
  — bidirectional now, pushing the internal clock out when the ground is the authority and
  pulling the DS1307 in otherwise. Two of that pass's five findings were stale, restating
  the depth-5 queues and the local reconnection that the commit under review had already
  fixed.
- **`pio check`**: 6 LOW findings, 0 MEDIUM, 0 HIGH, **none of them attributable to this
  change** — they are the pre-existing `src/logger.cpp` casts and unused labels,
  `src/mavlink.cpp`'s `voltages_ext` and one in `lib/Battery`. The numeric casts added here
  are not flagged; `cstyleCast` only reports pointer casts. Note the count does not match
  the 13 that the archived `add-degraded-mode` entry's task 9.5 recorded as the baseline,
  which predates this change and is not reconciled by it.

## 6. Documents and backlog

- [x] 6.1 Rewrite `ARCHITECTURE.md` §5.3 — which is already wrong, claiming `begin()`
  fails hard via `configASSERT` when `add-degraded-mode` made a missing clock a
  degradation — to describe the origin ladder, the sub-second source and the boot epoch.
  Fix §7's "the board runs on ticks since boot", which stops being the mechanism.
  Verified by reading both sections back against the delivered code.
- [x] 6.2 Delete `TODO.md`'s *Improve clock synchronisation* entry; remove only the
  **6.1 / 6.4** bullet from *Finish what add-degraded-mode left open*, leaving its other
  seven; and re-point *The flight log is a private MessagePack format no tool can read*
  so its `TIME` record takes `Src` from this change's origin values — it enumerates three
  and is missing `survived` — and so its plan to extend the tick "by an accumulator
  sampled at 1 Hz" is replaced by this change's elapsed-time accessor. Leave *`uptime`
  overflows after ~49.7 days* and *Decide whether `SYSTEM_TIME` deserves 1 Hz* untouched.
  Verified by a grep for each entry title and for `Src`.
- [x] 6.3 Record 1.3's drift figure and 1.4's flag answer where a follow-up can find
  them: a new `TODO.md` entry for selecting the sub-clock if, and only if, both
  measurements support it. Verified by the entry existing with the figures in it.
- [x] 6.4 Confirm the CI grep guards still pass — no `xTaskCreate`, `xQueueCreate` in
  `src/`, and no `pvPortMalloc` in `src/link.cpp` or `src/mavlink.cpp`, none of which
  this change introduces. Verified by the workflow run.

## 7. Close out

- [x] 7.1 **[board]** **[destructive]** Final pass in order: `pio run` on both
  environments, `pio test` (HIL, leaves the flight firmware running),
  `pio test -e libs` then `pio run -t upload` to leave the board operational. Record
  which rows above are closed on hardware and which are not — per `CLAUDE.md`, a step
  that could not be run is recorded as unrun rather than quietly dropped.

  **Done, in this order**: `pio run` on the flight environment and `libs` (both build);
  `pio test -e libs` twice — the first run failed on the `R64CNT` mask and the second
  passed 12/12 after the fix; `pio test` (HIL) re-run afterwards, which both reflashed the
  flight firmware and re-validated 25 of 33 cases against the corrected build. **The board
  is left running the flight firmware.** Seven rows remain open and each says why in its own
  line: 1.2 and 5.5 need a USB-TTL adapter on D0/D1; 3.4 and 5.4 cannot be closed on this
  assembly at all; 1.3 needs its second reading an hour after the first; 3.5 and 3.6 are
  each half-closed, waiting on 5.5 and 1.3 respectively.
