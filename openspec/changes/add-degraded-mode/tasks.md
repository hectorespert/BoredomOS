CI runs `pio run` on all three environments, the static-creation grep, and `pio check`.
Nothing else. This change is unusual in how little of it a build can prove: every
requirement is about a reset, a register that survives one, or a task set chosen at boot.
Steps marked **[board]** need the assembled board, and there are many — the board is
unreachable today, so `TODO.md`'s *Recover the bricked board* gates all of them.

**Revised after `review.md`.** This pass folds in findings 1, 2, 3, 3b, 3c, 3d, 4, 7, 8,
11, 13 and 14, and the register-ownership audit the verdict asked for as a single pass
over `include/Recovery.h` and section 1 (see `design.md`'s layout table). Every task
below now names the `test-plan.md` row(s) it satisfies, inverting the citation direction
`review.md` finding 11 flagged as backwards. Where `test-plan.md`'s own audit
(*Audit of the existing task list*, (a) and (b)) found a cited task's claimed
verification does not actually produce the scenario's receipt, that is marked inline as
**not resolved in this pass** — those are findings 5, 6, 9, 10, 12, 15, 16, 17, and audit
items (b)1 through (b)10, none of which `review.md`'s verdict lists as blocking apply.
Two new tasks (3.4, 7.5) close the two gaps finding 11 named as having no task at all.
The two `MODIFIED` capability deltas (`mavlink-link`, `memory-budget`) have since been
added under `specs/` via `openspec instructions` directly (`/opsx:continue` was not
installed in this environment) — `proposal.md` names them and `specs/` now holds them.

**Applied so far (this session, no board reachable).** 1.1, 1.2, 2.1 and 3.1 are
implemented and `pio run`-verified; 2.1 additionally resolved finding 3b outright, from
the RA4M1's own CMSIS register header rather than from an assumption — see `design.md`.
Implementing 2.1's counter logic also surfaced a real conflict between finding 3d's
"clamp on read" and finding 3's snapshot comparison (clamping hides growth once a
counter saturates at its threshold, which is exactly when the snapshot mechanism needs
to see it) — resolved by clamping only at the heartbeat-display boundary, not in the
stored/returned counts; `design.md` and this file's task 5.1 reflect the fix. Every
`[board]` task remains untouched: the board is unreachable, so none of them have been
attempted or claimed.

## 1. The shared declarations

- [x] 1.1 Add `include/Recovery.h` declaring the backup-register layout as one block,
  not field by field: `[0..3]` the bootloader's double-tap magic, never read or written
  by this firmware; `[4..5]` a validity magic byte and a checksum byte over `[6..11]`;
  `[6]` the consecutive-unstable-boot counter; `[7]` the cumulative-reset counter; `[8]`
  the cumulative-count snapshot taken on entering the reduced configuration; `[9]` the
  phase marker; `[10]` the reset reason (power-on, low-voltage, watchdog, software,
  external/unknown, or backup-state-invalid); `[11]` the deliberate-reset marker (none,
  commanded, retry). Also declare the boot-phase enumeration. Declarations only, no
  state — verify it compiles when included from both `src/main.cpp` and
  `src/mavlink.cpp` and that `pio run` still succeeds (supports every row indirectly;
  `review.md` findings 2, 3, 3d, 4 corrected)
- [x] 1.2 Add the `PRCR`-unlocked read and write helpers for `[4..11]`, making the
  unlock/write/lock sequence a critical section owned by the helper itself so no caller
  has to remember it — the same sequence runs from the USB interrupt
  (`SerialUSB.cpp:164-166`), and an ISR landing mid-sequence silently drops a write.
  Include the magic/checksum write on any update to `[6..11]`, and clamp both counters
  to their thresholds on every read regardless of the checksum outcome. Verify by
  inspection against `cores/arduino/boot.cpp`, the working precedent for the unlock
  sequence (`review.md` findings 3d, 7)

## 2. Read why the board restarted

- [x] 2.1 `review.md` finding 3b resolved from the RA4M1's own CMSIS register
  header (`R7FA4M1AB.h`): `PORF` is "writable only to clear... confirm the value is 1
  and then write 0", the same idiom as every other `RSTSR0`/`RSTSR1` flag, with no
  other documented auto-clear condition — see `design.md`. Read `RSTSR0`, `RSTSR1` and
  `RSTSR2` at the top of `setup()` and decode them into one of five reasons — power-on,
  low-voltage, watchdog, software, external/unknown (inferred by elimination once the
  other four are known-clear) — clearing `RSTSR0` and `RSTSR1` bit by bit with the
  confirm-1-then-write-0 idiom, and setting `RSTSR2.CWSF` to 1 for the next boot rather
  than clearing it, since it is software-set, not software-cleared. `pio run` succeeds
  and the decode covers all five reasons with a distinct value each (→ TP-4, TP-6;
  supersedes the four-reason, clear-everything version — `review.md` findings 3b, 3c,
  14 corrected). **Board confirmation (2.2, 2.3) still pending** — this is a compile-time
  verification only.
- [ ] 2.2 **[board]** Verify each cause is reported correctly: power the board (power-on),
  press RESET (external/unknown), trigger a software reset, starve a task to trip the
  watchdog, and — if a controlled low-voltage condition can be produced safely on the
  bench — brown it out; checking after each that the value the firmware reports matches
  what was done (→ TP-4, TP-6, and the low-voltage and external/unknown scenarios added
  to `specs/fault-recovery/spec.md`)
- [ ] 2.3 **[board]** Verify the registers are cleared: after a watchdog reset, the boot
  that follows the next clean start does not still report a watchdog reset — except for
  `RSTSR0.PORF`, which per 2.1's settled answer may be deliberately left uncleared (→ TP-4)

## 3. The phase marker

- [x] 3.1 Write the phase marker at each milestone of `setup()` — before anything, after
  the **link**, after the clock, after the card, after the queues, after the tasks, after
  the scheduler starts. The link now comes before the clock and the card (`review.md`
  finding 13, resolved in `design.md`): both the heartbeat and the boot `STATUSTEXT` need
  `LINK_SERIAL.begin()` to have run, so a hang at an earlier milestone in the original
  order was a permanent, undiagnosable reset loop. `pio run` succeeds and every milestone
  has a distinct value (→ TP-5)
- [ ] 3.2 Write a phase marker from both fault hooks in `src/hooks.cpp` before they
  blink, so a watchdog reset that follows a stack overflow or an allocation failure is
  attributable to it; verify `pio run` succeeds and the hooks still contain no `Serial`,
  no `delay()` and no interrupt-dependent wait (supports TP-3's phase-byte receipt;
  **The marker write is implemented and `pio run` succeeds; the verification clause
  cannot pass, so this stays unchecked. `review.md` audit (b)3 is not resolved in this
  pass**: `vApplicationStackOverflowHook` still contains `taskDISABLE_INTERRUPTS()`
  then `while (!Serial) {}` then two `delay(2000)` calls, contradicting this same
  file's own comment that `delay()` cannot be used from a fault hook, and putting the
  hook's own blink after a wait that never completes with interrupts disabled --
  `review.md` finding 9's fix, marker first and an immediate `NVIC_SystemReset()` with
  no blink, is not folded into this task and must land before this is checked off)
- [ ] 3.3 **[board]** Verify the marker names the right step: remove the SD card, let the
  board reset, and confirm the following boot reports the card phase (→ TP-5 **only
  until task 6.2 lands** — `review.md` audit (b)2: once 6.2 makes a missing card a
  degradation rather than a halt, pulling the card no longer produces a reset here, and
  this act and 6.2 become mutually exclusive as written. Re-scope this task to a phase
  hang that survives the degradation policy — e.g. a temporary build that stalls inside
  `SD.begin(9)` without triggering 6.2's absence path — before relying on it as TP-5's
  receipt. **Not resolved in this pass.**)
- [ ] 3.4 **[board]** Provoke `vApplicationStackOverflowHook`: flash a temporary build
  with an undersized stack behind a deliberate deep call, confirm the board resets
  within `WDT_TIMEOUT_MS`, confirm the following boot's phase byte equals that hook's
  marker, and reflash the flight build afterward (→ TP-3; `review.md` finding 11's own
  disposition named this as a scenario with no task at all — this is that task, and its
  receipt is conditional on 3.2's caveat above being resolved first)

## 4. The watchdog

- [x] 4.1 Add `-D WDT_TIMEOUT_MS` to `platformio.ini`, sized only against
  `TaskSdWrite`'s worst case (ceiling 5.592 s, see `design.md`) — no longer against the
  upload path, which no longer depends on the watchdog (see 4.2). Open the WDT in
  `setup()`, before the first phase milestone, with `reset_control` set to reset. Verify
  `pio run` succeeds (→ TP-2)
- [x] 4.2 Override `vApplicationIdleHook()` in `src/hooks.cpp`: test
  `is_watchdog_reset_in_progress_for_upload` (`SerialUSB.cpp:160`) first, and if set call
  `goBootloader()` (`cores/arduino/boot.h`) directly instead of refreshing — this
  supersedes the original design of relying on watchdog underflow to reach the
  bootloader, which `review.md` finding 1 shows cannot fit under `dfu-util`'s 1000 ms
  detach timeout at any expressible watchdog period. Otherwise refresh the watchdog,
  reproducing the port's weak implementation around it — `__disable_irq()`,
  `vTaskSuspendAll()`, `rm_freertos_port_sleep_preserving_lpm(1)`, `__enable_irq()`,
  `xTaskResumeAll()` — with the flag test and refresh both before the sleep. Verify by
  comparing against `portable/FSP/port.c` that no part of the sleep sequence was dropped:
  losing it costs low-power idle permanently (this is what keeps `pio run -t upload`
  working at all; no TP row names it directly, since every board row depends on
  reflashing the board afterward)
- [ ] 4.3 **[board]** Verify normal operation never trips it: run the board for longer
  than several `TaskSdWrite` cycles with the card present and confirm no reset occurs
  (→ TP-2)
- [ ] 4.4 **[board]** Verify a hang does trip it: stall a task in a non-yielding loop and
  confirm the board resets within the timeout and reports a watchdog cause (→ TP-1)
- [ ] 4.5 **[board]** Verify `pio run -t upload` still works with the watchdog enabled
  and 4.2's idle-hook fix, and confirm it is **not** slower than today — 4.2 removes the
  timing dependency on the watchdog entirely, so this is a correctness check, not the
  latency measurement originally scoped here. Record the observed upload time in the
  commit message (`review.md` finding 1's disposition retires this task's original scope)
- [ ] 4.6 **[board]** Raise `WDT_TIMEOUT_MS` to the value `TaskSdWrite` actually needs,
  derived from 4.3's observations including a forced ring rollover — not guessed — and
  record the measured value in `design.md` beside the flag, not only in the commit
  message. If the measurement exceeds the 5.592 s ceiling, the fix is a smaller ring
  file, not a larger flag. Repeat 4.3 and 4.5 (→ TP-2; `review.md` audit (b)7 corrected)

## 5. The counters and the boot decision

- [x] 5.1 Add the consecutive-unstable-boot counter and the cumulative reset counter,
  advancing the consecutive one on watchdog and software resets only — excluding a
  software reset whose deliberate-reset marker reads *commanded* or *retry* — and both
  counters on every reset. Consume and clear the deliberate-reset marker as part of this
  read. Verify `pio run` succeeds and that a power-on, external/unknown, low-voltage or
  deliberate reset leaves the consecutive counter untouched (→ TP-7, TP-8; supports the
  new deliberate-reset scenario in `specs/fault-recovery/spec.md`; `review.md` finding 4
  corrected)
- [x] 5.2 Clear the consecutive counter once the firmware has run past the stability
  window (five minutes, derived in `design.md`), from the existing periodic path rather
  than a new task, and verify `pio run` succeeds and no new task or queue was created
  (→ TP-9; **`review.md` finding 16 is not resolved in this pass**: only
  `TaskHeartbeat` runs in every configuration, so if this clear or 8.2's retry countdown
  ever land on a task that does not run reduced, the board could never clear its counter
  or retry — this task should name `TaskHeartbeat` explicitly rather than "the existing
  periodic path", and does not yet)
- [x] 5.3 Make the task set conditional in `setup()`: on three consecutive unstable boots
  or ten cumulative resets, snapshot the current cumulative count into layout byte `[8]`
  (`review.md` finding 3) and start only the link reader, the link writer, the heartbeat
  and the protocol handler. Verify `pio run` succeeds, that the static storage for every
  task is still declared and counted whether or not it is started, and that
  `scripts/ram_budget.py` reports unchanged headroom (supports TP-7, TP-8, TP-10, TP-11,
  TP-17 — the boot-decision mechanism those rows exercise)
- [ ] 5.4 **[board]** Verify the trigger: force three consecutive resets before the
  stability window using either three watchdog resets from a stall build, or three
  software resets from a path whose deliberate-reset marker is left unset — not the
  RESET button, which the requirement forbids from advancing the count, and not a power
  cycle, which may clear `VBTBKR` entirely (see 9.7) — and confirm the fourth boot
  starts reduced (→ TP-7; `review.md` audit (b)4 corrected)
- [ ] 5.5 **[board]** Verify the cumulative path: reset repeatedly, each time after the
  stability window has passed, and confirm the reduced configuration is still reached
  (→ TP-8)
- [ ] 5.6 **[board]** Verify a normal boot clears the consecutive counter and the next
  boot is normal (→ TP-9)

## 6. Absent hardware degrades

- [ ] 6.1 Replace the RTC `configASSERT` with a degradation: record the absence, run on
  ticks since boot, and report epoch time as unset rather than as zero. Verify `pio run`
  succeeds and that `src/mavlink.cpp`'s existing inbound `SYSTEM_TIME` path still sets
  the clock (→ TP-12; **`review.md` audit (b)1 is not resolved in this pass**:
  `lib/SystemTime.cpp:11`'s `if (_ds1307.begin() && RTC.begin())` short-circuits, so the
  internal RTC never begins with the DS1307 absent, and `setUnixTime()` unconditionally
  calls `_ds1307.adjust()` on hardware that is not there — a `lib/SystemTime` change
  making the internal RTC begin regardless, and skipping the DS1307 write when absent,
  is still needed before TP-12's third receipt is obtainable)
- [x] 6.2 Replace the SD `configASSERT` with a degradation: record the absence and do not
  start `TaskLogger` or `TaskSdWrite`. Verify `pio run` succeeds (→ TP-13; see 3.3's
  caveat above — this task's absence policy is what invalidates 3.3's card-pull act as a
  phase-marker demonstration)
- [x] 6.3 The queue and task `configASSERT`s are left in place in `src/main.cpp`, as
  argument checks. `ARCHITECTURE.md` §7's `configASSERT` paragraph now documents that
  they stay for that reason -- static creation cannot fail for want of memory -- while
  the RTC and SD asserts they used to sit beside are gone
- [ ] 6.4 **[board]** Verify with no RTC attached: the board reaches its steady-state
  cadence, reports the absence, and accepts a time set from the ground (→ TP-12, subject
  to 6.1's caveat)
- [ ] 6.5 **[board]** Verify with no card attached: the board reaches its steady-state
  cadence, reports the absence, and telemetry continues at normal rates (→ TP-13)
- [ ] 6.6 **[board]** Verify with neither attached, in the reduced configuration: the
  board still transmits and still receives (→ TP-11)

## 7. Tell the ground

- [x] 7.1 Pack the reset reason, the phase reached and both counters into the heartbeat's
  `custom_mode`, which is sent as zero today, and set `system_status` to
  `MAV_STATE_CRITICAL` and drop `MAV_MODE_FLAG_AUTO_ENABLED` from `base_mode` while
  reduced. Verify `pio run` succeeds and that the identity triple is unchanged (supports
  TP-7, TP-9, TP-10, TP-14, TP-15 — the fields those rows read)
- [x] 7.2 Emit one `STATUSTEXT` at `MAV_SEVERITY_CRITICAL` at boot naming the suspected
  cause, and verify it fits the 50-character field without the pointer-arithmetic defect
  that `TODO.md`'s *Fix the pointer arithmetic in the unknown-message `STATUSTEXT`*
  describes — do not reproduce that pattern here (supports TP-3, TP-4, TP-5, TP-6,
  TP-12, TP-13 — the boot `STATUSTEXT` those rows read)
- [ ] 7.3 **[board]** Verify with a **passive** listener (`run.py`, not MAVProxy —
  MAVProxy transmits its own heartbeats and a parameter fetch on connect, which cannot
  evidence "having issued no request") that a ground station connecting long after a
  degraded boot sees the reduced state, the cause, the phase and both counters in the
  first heartbeat it receives, with a capture containing no host-originated frame before
  it (→ TP-14; `review.md` audit (b)5 corrected — receipt is `check_recovery.py`'s
  `test_first_heartbeat_reports_reduced_state:PASS`, added in 7.5)
- [ ] 7.4 **[board]** Verify normal operation reports as operational, and that the
  message set and rates match the twelve HIL cases -- the nine pre-existing plus
  `check_recovery.py`'s three, added in 7.5 (→ TP-15)
- [x] 7.5 Add `test/test_hil/check_recovery.py` with three passive cases:
  `test_first_heartbeat_reports_reduced_state` (asserts, on the *first* `HEARTBEAT` a
  passive listener sees, `MAV_STATE_CRITICAL`, `base_mode` without `AUTO_ENABLED`, and a
  `custom_mode` that decodes to a valid reason, phase and both counts, with no
  host-originated frame before it in the capture); `test_heartbeat_reports_operational`
  (asserts `MAV_STATE_ACTIVE`, `AUTO_ENABLED` set, reduced bit clear); and
  `test_observed_message_set_is_closed` (asserts a 12 s sample contains exactly
  `HEARTBEAT`, `SYSTEM_TIME`, `BATTERY_STATUS`, plus `COMMAND_ACK` once 8.1 lands — see
  8.1's note). Both degraded-state cases must raise `hil.NoLinkError` when the board is
  not in the state they check, so a healthy-board run reports `IGNORE` rather than
  `FAIL`. Update `test/test_hil/README.md`, record which scenario each case covers (see
  `TODO.md`'s *Record which scenario each HIL case covers*), and correct this file's own
  two hard-coded "nine HIL cases" references (7.4 above and 9.6 below) once this lands.
  Verify `pio test` still exits 0 with these three cases `IGNORE`d on a board not in the
  state they check (→ TP-3 partially, TP-14, TP-15; `review.md` finding 11's own
  disposition named "the `check_recovery.py` work" as having no task at all — this is
  that task, alongside 3.4)

## 8. Leave the reduced configuration

- [x] 8.1 Act on `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` in the existing `COMMAND_LONG` case:
  write the deliberate-reset marker as *commanded*, clear both counters and the
  cumulative-count snapshot, and reset. Verify `pio run` succeeds and that the command is
  acknowledged with a `COMMAND_ACK` — a message not on the wire today, which must be
  added to `check_recovery.py`'s closed-set expectation from 7.5 (`review.md` audit
  (b)10) — rather than silently handled (→ TP-16)
- [x] 8.2 Add the long automatic retry so a board whose link is the failing part is not
  stranded: write the deliberate-reset marker as *retry* before resetting, with an
  interval long enough that it cannot oscillate (thirty minutes, derived in
  `design.md`). Verify `pio run` succeeds and the interval is a named constant, not a
  literal (→ TP-17; **`review.md` finding 16 caveat also applies here** — name
  `TaskHeartbeat` explicitly as the task carrying this countdown, not yet done)
- [ ] 8.3 **[board]** Verify the ground command returns the board to normal (→ TP-16)
- [ ] 8.4 **[board]** Verify the automatic retry fires after the interval, and that a
  board that fails again returns to the reduced configuration rather than cycling
  rapidly — the receipt is the interval between successive entries into the reduced
  configuration being at least the retry interval, per the reworded scenario in
  `specs/fault-recovery/spec.md` (→ TP-17)

## 9. Documents, backlog and verification

- [x] 9.1 Updated `ARCHITECTURE.md`: the boot decision and the reduced task set (§3),
  the watchdog and the backup registers in the resource table (§6), the rewritten
  `configASSERT` policy and the corrected MAVLink frame-size figures (§7 --
  `review.md` audit item under finding 17: the 68-byte/11.8 ms `BATTERY_STATUS`
  figure now reads 36-byte payload, 48-byte frame, 8.33 ms), and the headroom and
  HIL-case-count figures (§8, including the finding-17 correction that the cases
  no longer follow only `mavlink-link`). §3 explicitly notes that finding 9's
  fault-hook rewrite is *not* folded into this change, so the document does not
  claim it as done
- [x] 9.2 Rewrote the `configASSERT` convention in `CLAUDE.md` rather than amending
  it, and added that `VBTBKR[0..3]` belongs to the bootloader's double-tap magic and
  must never be written by this firmware (`review.md` finding 2 corrected)
- [x] 9.3 Verified `openspec/config.yaml`'s own `context` block independently: the
  sentence originally targeted here (`configASSERT` halts on a missing RTC or SD card)
  turned out to live in `CLAUDE.md`, not here (task 9.2 handles that copy) — `grep`
  confirms `config.yaml` never states that claim in any wording, so there is nothing
  to correct here. This step was a no-op against a file that never held the sentence,
  now confirmed rather than assumed (`review.md` audit item under finding 17 corrected)
- [x] 9.4 Deleted the three `TODO.md` entries this change consumes — *Add a
  watchdog*, *`setup()` asserts on the RTC before the console exists*, and *Report
  the satellite's real state in the heartbeat* — and re-pointed every
  cross-reference at `openspec/changes/add-degraded-mode` (six sites: *The stack
  overflow hook hangs before it warns*, *`uptime` overflows after ~49.7 days*,
  *Emit `SYS_STATUS`*, *SD logging failure is silent*, *Review the contents of the
  messages already emitted*, *Recover the bricked board*), plus one more accuracy
  fix surfaced along the way (*SD logging failure is silent* also claimed
  `configASSERT(SD.begin(9))` still halts, which this change made false).
  `grep` confirms no `*[...]*`-style link to any of the three remains
- [ ] 9.5 `openspec validate add-degraded-mode --strict` passes; `pio run` succeeds
  on all three environments; the static-creation grep finds nothing. `pio check`
  reports 13 LOW findings, not the 12 baseline: the one new hit
  (`src/mavlink.cpp:336`, a C-style pointer cast in the new `sendCommandAck()`) is
  the same style every other `pvPortMalloc` cast in this file already uses, not a
  new class of defect, but it is a real change to the count and this task's own
  wording promises none -- left unchecked rather than silently accepted. A
  `static_cast` there alone would read as inconsistent against the six identical
  casts already in the file; whether to convert all of them, accept the one new
  hit, or suppress the rule is a call for review, not for this pass to make alone
- [ ] 9.6 **[board]** Clear both counters and the deliberate-reset marker immediately
  before this run — every flash and every `pio test` produces a watchdog reset that
  advances the cumulative count, and around the tenth the board boots reduced, failing
  `test_battery_status_every_2s` for a legitimate reason. Run `pio test` and verify all
  nine pre-existing HIL cases pass in the normal configuration, plus
  `check_recovery.py`'s `test_heartbeat_reports_operational` and
  `test_observed_message_set_is_closed` (7.5), which also apply to a normal boot and
  should `PASS`; only `test_first_heartbeat_reports_reduced_state` self-skips to
  `IGNORE` on a board that is not reduced. Record the counter values read from the
  first heartbeat alongside the PASS lines, so the run's context is on the receipt.
  Without a board every case reports `IGNORE` and the command exits 0, so a green run
  off the board proves nothing (→ TP-15; `review.md` audit (b)8 corrected)
- [ ] 9.7 **[board]** Verify `VBTBKR` survives what this change needs it to: reset the
  board and confirm the counters persist. Then remove power entirely and confirm whether
  they do — the design does not depend on that for a normal reset, but the auto-retry's
  snapshot-based comparison (finding 3) and the auto-retry interval must not be trusted
  across a power cycle until this is known either way. Record the outcome in `design.md`'s
  Risks section, and make TP-17's receipt reference it (`review.md` audit (b)9 corrected)
- [ ] 9.8 **[board]** Clear both counters after commissioning, since they survive
  reflashing and a firmware flashed over a board that had counted failures starts from
  that count
