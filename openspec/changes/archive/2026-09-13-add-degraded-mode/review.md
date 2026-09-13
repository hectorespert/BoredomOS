## Reviewers

Six agents, spawned in parallel, each given the change directory and the repository. Three
ran on opus and three on sonnet, as an experiment in cost — see the closing note.

| Agent | Model | Cost | Outcome |
|---|---|---|---|
| `hardware-engineer` | opus | 196 534 tok / 78 tools | 17 findings. Disassembled `libfsp.a` and the Minima bootloader image itself. Ran twice; the first two attempts were lost to a session limit before this one completed. |
| `firmware-engineer` | opus | 149 888 tok / 68 tools | 20 findings. Disassembled the shipped `libfsp.a`. |
| `systems-engineer` | sonnet | 140 446 tok / 34 tools | 8 findings. Ran `pio run` and `pio check` to re-derive two figures. |
| `qa-engineer` | opus | 112 452 tok / 43 tools | 19 findings. Stood aside from `test-plan.md`, which it wrote. |
| `operations-engineer` | sonnet | 60 028 tok / 12 tools | 7 findings, two of them serious. |
| `payload-scientist` | sonnet | 59 076 tok / 12 tools | 4 findings. Not out of domain, contrary to expectation. |

Every claim reproduced below was checked against the tree before being recorded here, except
where marked as resting on the hardware engineer's disassembly of the bootloader image, which
is not in this repository and could not be independently reproduced under this session's
remaining budget — see finding 3. Where a reviewer was wrong or two disagreed, that is in
*Disagreements*.

## Findings

Ranked across all six, duplicates merged, most serious first.

### 1. The firmware could not be reflashed over USB

**Where:** `design.md:106-110`, `proposal.md:115-119`, `tasks.md:58-61` (4.5)
**Found by:** firmware-engineer. **Verified here.**

The design argues the DFU upload still works because `tud_dfu_runtime_reboot_to_dfu_cb`
spins when `R_WDT_Open` returns `FSP_ERR_ALREADY_OPEN`. That branch is dead:

```c
// cores/arduino/usb/SerialUSB.cpp:174-182
int err = R_WDT_Open(&p_ctrl, &p_cfg);
R_WDT_Refresh(&p_ctrl);
is_watchdog_reset_in_progress_for_upload = true;
if (err == FSP_ERR_ALREADY_OPEN) { while (1); }
```

`BSP_CFG_PARAM_CHECKING_ENABLE` is `0` (`variants/MINIMA/includes/ra_cfg/fsp_cfg/bsp/bsp_cfg.h:28`)
and `WDT_CFG_PARAM_CHECKING_ENABLE` inherits it (`r_wdt_cfg.h:8`), so the `FSP_ERROR_RETURN`
that would produce that code is compiled out. The callback refreshes the application's
watchdog, sets the flag, and returns. Tasks resume, the idle hook resumes refreshing, the
watchdog never fires, and the reset into the bootloader never happens.

**Why it matters:** this change's own motivating incident is a board that needed physical
access. It would ship a firmware that cannot be flashed over USB — and there is a board in
DFU right now that got there somehow.

**The core already provides the answer**, and the change does not use it:
`is_watchdog_reset_in_progress_for_upload` (`SerialUSB.cpp:160`) exists for exactly this,
and the port's weak hook tests it at `port.c:1482-1484`. Task 4.2 transcribes that sequence
and omits the flag test.

**Disposition: open — blocks apply, and the fix above is not sufficient.** The hardware
engineer's later run (see below) shows that even with the flag test added, relying on WDT
underflow to reach the bootloader cannot work: the DFU-RT descriptor's `wDetachTimeOut` is
1000 ms (`USB.cpp:145`, `TUD_DFU_RT_DESCRIPTOR(..., 0x0d, 1000, 4096)`; **verified here**),
and the shortest timeout the hardware can express is 174.8 ms but the change needs headroom
above `TaskSdWrite`'s worst case (finding 5), which will not fit under 1 s. `dfu-util` gives
up before the watchdog would fire.

**Adopted fix, superseding the one above:** do not use the watchdog as the DFU trigger at
all. In the overridden `vApplicationIdleHook()`, test the flag and jump to the bootloader
directly instead of merely skipping the refresh:

```c
extern bool is_watchdog_reset_in_progress_for_upload;
if (is_watchdog_reset_in_progress_for_upload) goBootloader();  // cores/arduino/boot.h
R_WDT_Refresh(&wdtCtrl);
```

`goBootloader()` rewrites the magic and calls `NVIC_SystemReset()` directly — microseconds,
not seconds. This also removes the entire justification for `WDT_TIMEOUT_MS` being tied to
the upload path, and makes upload faster than today's ~175 ms rather than slower, which
retires task 4.5 as originally scoped.

### 2. The bootloader owns `VBTBKR[0..3]`, not `VBTBKR[0]`

**Where:** `proposal.md:25-26`, `design.md:14,17-18`, `tasks.md:9-13` (1.1), `tasks.md:137-139` (9.2), and `TODO.md:1022`
**Found by:** qa-engineer and firmware-engineer, independently. **Verified here.**

```c
// cores/arduino/boot.h:14
#define BOOT_DOUBLE_TAP_DATA (*((volatile uint32_t *) &R_SYSTEM->VBTBKR[0]))
```

`VBTBKR` is `__IOM uint8_t VBTBKR[512]` and `DOUBLE_TAP_MAGIC` is 32 bits, so the magic
occupies bytes 0 through 3. The claim "the bootloader uses exactly one of them... the other
511 are untouched" is wrong by three, and a counter at `VBTBKR[1]` corrupts the double-tap
magic — the only recovery path for a board with no working firmware.

This error originated in this session's exploration, was propagated into the proposal, the
design, two tasks and a backlog entry, and survived until two independent reviewers caught
it.

**Disposition: open — blocks apply.** Reserve `[0..3]`, start the layout at `[4]`, and
correct all five sites in one commit.

### 3. A board reduced by the cumulative counter can never leave

**Where:** `specs/fault-recovery/spec.md:64-68` and `:172-175`, `design.md:118-122`, `:134-137`
**Found by:** operations-engineer and firmware-engineer, independently. **Verified here.**

The spec requires a cumulative count "that only a ground command clears", and that the
firmware start reduced "when **either** count passes its threshold". R7 then requires it to
"leave the reduced configuration on its own after a long interval, so that a firmware whose
link is the failing part is not stranded in it permanently".

Walk the case R7 names. No command can arrive — that is the premise. The auto-retry resets
the board; the reset itself is recorded in the cumulative count (`design.md:134-137`); the
next boot re-evaluates the raw threshold, finds it still exceeded, and returns to reduced
before emitting a heartbeat. **R7's second sentence is unsatisfiable for exactly the case it
was written for**, and each attempt deepens the trap. Two requirements of the same delta
contradict each other, and this becomes contract on archive.

`TP-17` does not catch it: its act keeps the inducing fault present, which is the
consecutive path, not the cumulative one.

**Disposition: open — blocks apply.** The fix both reviewers converge on is state, not
wording: `design.md:16` says 511 bytes of `VBTBKR` are free — snapshot the cumulative count
on entering reduced mode and gate the boot decision on growth since that snapshot. See also
finding 4, which is the same missing state.

### 3b. `RSTSR0.PORF` may gate the bootloader's DFU-arming window — unverified, and clearing it may be irreversible

**Where:** `tasks.md:20-23` (2.1)
**Found by:** hardware-engineer, from disassembling `bootloaders/UNO_R4/dfu_minima.hex`. **Not
independently reproduced** — that file is not in this repository and re-disassembling it was
outside this session's remaining budget. Recorded as an unverified hardware claim, which is
this role's distinctive kind of finding, not as an established fact.

The claim: on every boot without the double-tap magic set, the bootloader reads
`RSTSR0.PORF` before deciding whether to arm its 500 ms DFU window, at a fixed offset in the
image. Task 2.1 clears `RSTSR0` on every boot. If `PORF` is hardware-cleared by any
non-power-on reset, this is harmless. If it is sticky until software clears it — which is
consistent with the RA4M1's documented behaviour for other `RSTSR` flags, and with why
`design.md:7-11` says "they accumulate until cleared" — then either the window has never been
armed on any boot after the first (today's status quo, harmless to preserve), or clearing it
newly arms a 500 ms DFU window after every reset for the rest of the mission on a satellite
with no USB host once flying.

**Disposition: open — must be settled before 2.1 is written, not after.** Two ways to settle
it, cheapest first: the RA4M1 hardware manual's `RSTSR0` register description and its
reset-behaviour column for `PORF`; or, on the board, read `RSTSR0` in `setup()` across a
power-on and a RESET-press boot **without clearing anything**, and compare. Whichever the
answer, 2.1 must say explicitly whether `PORF` is latched-and-preserved or cleared alongside
the rest, and `design.md`'s Risks must record the DFU-arming consequence either way.

### 3c. There is no reset-reason flag for the RESET pin

**Where:** `proposal.md:22`, `spec.md:38-40`, `tasks.md:23`
**Found by:** hardware-engineer.

`RSTSR0` carries only `PORF` and the LVD/deep-standby flags; `RSTSR1` carries the
independent-watchdog, watchdog, software-reset and bus/parity/stack-monitor flags; `RSTSR2`
carries only the cold/warm-start flag. **A RES-pin reset sets none of them.** It is only
inferable by elimination, and only if every other flag was known-cleared beforehand — which
finding 3b says must not be done unconditionally. `RSTSR2.CWSF` is the intended cold/warm
discriminator and is *set by software*, not cleared: task 2.1's "clear them" is backwards for
this one register.

**Disposition: open.** Reword the requirement to "power-on, watchdog, software, or none of
these (inferred external/unknown)". Split 2.1's register handling: latch all three, clear
`RSTSR1` per its own read-clears-that-bit semantics, set `RSTSR2.CWSF` to 1 for the next
boot, and resolve `RSTSR0.PORF` per finding 3b rather than clearing it unconditionally.

### 3d. Backup-register content is read as counters with no validity marker

**Where:** `tasks.md` §1, §5
**Found by:** hardware-engineer.

Nothing writes a magic or a checksum before the layout is read as two counters. Three
consequences: the board is in DFU right now, about to have `VBTBKR[0..3]` zeroed by the next
boot into the bootloader per finding 2 — but `[4..511]` hold whatever the last power event
left, undefined by anything in this change, so **the first boot after flashing reads
uninitialised bytes as both counters**, and if they happen to exceed the thresholds the board
starts reduced immediately, before task 9.8's "clear after commissioning" runs. Separately,
in orbit, an SEU or a brownout interrupting an unprotected read-modify-write could set the
cumulative counter past ten, permanently selecting the reduced configuration with the SD log
off, with no way to distinguish that from ten genuine faults.

**Disposition: open.** Reserve a word for a magic plus a one-byte checksum over the block; on
mismatch, treat the block as zero and record a distinct reason. Clamp both counters to their
thresholds on read regardless.

### 4. Every deliberate reset is recorded as a fault

**Where:** `design.md:135-137`, `tasks.md:122-127` (8.1, 8.2)
**Found by:** firmware-engineer.

Software resets advance the consecutive counter by design decision. Both exits from the
reduced configuration are software resets. So the commanded exit leaves the next boot at
consecutive = 1, and **three commanded reboots put the board into the reduced configuration
by themselves** — the opposite of what the command is for. The auto-retry's own reset is
likewise counted as evidence of the fault it is testing for.

**Disposition: open — blocks apply.** One more field in the 1.1 layout: a deliberate-reset
marker written before `NVIC_SystemReset()` and consumed by the next boot. With a value
distinguishing *commanded* from *retry*, it also supplies the state finding 3 needs.

### 5. `WDT_TIMEOUT_MS` has a ceiling of 5.592 s, and the binding constraint is every SD write

**Where:** `tasks.md:62-63` (4.6), `design.md:98-104`, `:223-230`
**Found by:** firmware-engineer. **Arithmetic verified here.**

Max timeout is 16384 cycles at CLK/8192; `BSP_CFG_ICLK_DIV` is /1 and `BSP_CFG_PCLKB_DIV`
is /2 (`bsp_clock_cfg.h:12,14`), so PCLKB is 24 MHz and the ceiling is
134 217 728 / 24e6 = **5.592 s**. The value is also discrete — seven timeouts by ten
dividers — so a figure in milliseconds cannot be honoured exactly and the code must round.

The design frames the SD card as a wedge case. It is not. `TaskSdWrite` runs at
`PRIORITY_LOWEST` = `tskIDLE_PRIORITY` with `configUSE_TIME_SLICING` at 0, so while it
spins the idle task does not run and the watchdog is not refreshed. **Every ordinary
`sdData.write()` must complete inside the timeout**, and the worst is the ring rollover at
`lib/SdData/SdData.cpp:56-68`, which deletes a file of up to 1 GiB inside one call.

**Disposition: open — must be resolved before 4.6 is written.** Put the ceiling in
`design.md`; make 4.6 a bounded measurement including a forced rollover. If the measurement
exceeds the ceiling the answer is a smaller ring file, not a larger flag.

### 6. Opening the watchdog does not start it, and `setup()` has no room under the ceiling anyway

**Where:** `tasks.md:45-47` (4.1), `:40-41` (3.3), `spec.md:36-59`
**Found by:** firmware-engineer, contradicting the test plan's audit (a)4; **confirmed by
hardware-engineer**, resolving *Disagreements* item 1.

`R_WDT_Open` writes WDTRCR, WDTCR and WDTCSTPR and never WDTRR; in register-start mode the
counter begins on the first refresh, and nothing refreshes between `setup()`'s open and
`vTaskStartScheduler()`. So a hang during initialisation produces no reset, R2's second
scenario and TP-5 have no mechanism, and the phase marker is never read back. The hardware
engineer independently reached the same reading from the register side and adds the number
that makes the fix urgent: `SD.begin(9)` is bounded by `SD_INIT_TIMEOUT = 2000 ms`, used
twice, and `systemTime.begin()` carries a further `delay(200)` in `RTC.begin()` — a worst-case
`setup()` comfortably over 2.5 s against the 5.592 s ceiling from finding 5, with no slack
for a "deliberately short value" as 4.1 currently asks for.

**Disposition: open.** The proposed fix is nearly free and improves the obligation: refresh
at each phase-marker write in 3.1. The marker points are the right refresh points, and the
requirement weakens from "all of `setup()` fits in the timeout" to "no single step exceeds
it", which is both testable and, per the measurement above, achievable.

### 7. Writing the backup registers races the USB interrupt over `PRCR`

**Where:** `design.md:19`, `tasks.md:14-16` (1.2)
**Found by:** firmware-engineer.

`PRCR` is one global lock with a non-atomic unlock/write/lock sequence, and the core
performs that sequence **from the USB interrupt** (`SerialUSB.cpp:164-166`). An ISR landing
between the application's unlock and its store makes the store a silent no-op — losing a
counter update exactly when a host is touching the port.

**Disposition: open.** Make the critical section a property of the 1.2 helpers so no caller
has to remember. This is the only genuinely concurrent new state the change introduces, and
the design should say so.

### 8. Two archived capabilities are falsified, and the proposal declares neither

**Where:** `proposal.md:86-90`; `openspec/specs/mavlink-link/spec.md`, `openspec/specs/memory-budget/spec.md`
**Found by:** qa-engineer and systems-engineer, each finding a different one. **Verified here.**

- `mavlink-link`, *Board powered with nothing attached*: "all tasks reach their steady-state
  cadence" and "housekeeping records continue to be written to the SD card at 1 Hz" — false
  in the reduced configuration and false with no card.
- `memory-budget`, *A ground station sees no difference*: requires the message set at the
  rates `mavlink-link` defines; the reduced configuration drops `BATTERY_STATUS`.

The proposal says *Modified Capabilities: None* and mentions `memory-budget` only to defend
a different requirement.

**Disposition: open — blocks apply.** Two MODIFIED deltas qualifying each scenario to the
normal configuration, cross-referenced to `fault-recovery`. The systems engineer's wording
is adopted.

### 9. The fault-indication receipt in `memory-budget` stops existing

**Where:** `openspec/specs/memory-budget/spec.md:99`, `src/hooks.cpp:26-31` and `:59-67`, `tasks.md:45-47`
**Found by:** qa-engineer, extended by firmware-engineer.

`memory-budget` requires that "an observer can tell from the indication alone that this is
an allocation failure and not a stack overflow". The two patterns are a ~1.48 s cycle and a
4 s cycle. With the watchdog open at 4.1's "deliberately short value" — and at most 5.592 s
by finding 5 — neither completes one cycle before the reset. The receipt disappears with no
delta behind it.

**Disposition: open.** The firmware engineer's stronger version is the better answer and I
adopt it: both hooks write the phase marker as their **first** statement and then
`NVIC_SystemReset()` immediately — no `pinMode`, no blink, no `while(1)`. A pattern nobody
can see in orbit is worth less than a marker plus a reset reason, and this also disposes of
the `while (!Serial) {}` deadlock the test plan found rather than reordering around it.
`ARCHITECTURE.md:293-303` and `:335-339` then need rewriting, which task 9.1 does not list.

### 10. None of the new diagnostic state ever reaches the SD log

**Where:** `proposal.md:94-121`, `include/Data.h`, `tasks.md:106-113`
**Found by:** payload-scientist, seconded by operations-engineer and firmware-engineer.

The reset reason, phase and counters go only into `custom_mode` — a heartbeat someone must
be listening to at that moment. The Impact section lists neither `include/Data.h` nor
`src/logger.cpp`. Ground contact is passes, not continuous, and the incident that motivates
the whole change is a failure nobody was watching. Once a later boot advances the marker,
the earlier cause is gone.

**Disposition: open.** Four bytes in `Data`'s `System` block and a read in `src/logger.cpp`.
Note this interacts with finding 11: adding fields is only useful if they are assigned —
`Data::energy` is the standing example of one that is not, and a zero there reads as a dead
cell rather than as silence.

### 11. `tasks.md` names not one test-plan row

**Where:** `tasks.md`, all 42 steps. `grep -c 'TP-'` returns 0.
**Found by:** qa-engineer. **Verified here.**

`openspec/config.yaml` requires that every row of `test-plan.md` be named by at least one
task, because that is the chain a coverage audit walks and the one `verify.md` reads back.
The plan compensated by having each row cite a task, which is the reverse direction and
breaks the moment a task is renumbered.

This is a defect in my own transcription, against a rule added two commits earlier.

**Disposition: open — must be fixed before apply**, since it is the chain `verify.md`
depends on. TP-3 and the `check_recovery.py` work have no task at all and need one.

### 12. The `NULL` handle hazard, and the right shape of its fix

**Where:** `src/logger.cpp:31-37`
**Raised by:** the test plan; **judged by** firmware-engineer and payload-scientist. See *Disagreements*.

`prvGetTCBFromHandle(NULL)` is `pxCurrentTCB` (`tasks.c:234`), so a conditional `setup()`
that leaves a handle null makes the log report `TaskLogger`'s own mark seven times, silently
corrupting the only evidence this project has for sizing stacks.

**Disposition: open.** I adopt the firmware engineer's fix over the test plan's: a null-safe
wrapper in `src/logger.cpp` returning 0, with `0` documented in `include/Data.h` as "not
started". The invariant the plan proposed couples the boot decision tree to a file three
modules away, is checkable by no build and no test, and forbids the most useful future
subset — reduced *with* the card, so the log of a reduced boot can be read.

### 13. The phase marker is unreadable in the failure it exists for

**Where:** `spec.md:51-54`, `:76-79`, `tasks.md:74-78` (5.3)
**Found by:** qa-engineer.

Both the heartbeat and the boot `STATUSTEXT` need `setup()` to reach
`LINK_SERIAL.begin(LINK_BAUD)`. A hang at the clock phase happens before the link exists and
recurs identically on every boot including the reduced one, because nothing in the change
makes the reduced configuration skip an initialisation step. So a persistent pre-link hang
is a permanent reset loop the ground can never diagnose, while the spec promises the step
will be named.

**Disposition: open.** 5.3 must say whether the reduced boot skips `systemTime.begin()` and
`SD.begin(9)`; if it does not, the reduced configuration cannot escape the failure that
selected it.

### 14. No reset reason for a brownout

**Where:** `spec.md:38-40` against `design.md:7`
**Found by:** operations-engineer.

The design records that `RSTSR0` carries "the power-on **and low-voltage** flags". The spec
requires exactly four reasons and has no category for a low-voltage reset. On a satellite
running from one LiPo cell with a naive charge estimate, that is the cause most worth
telling apart from an ordinary power-on. A `uint8_t` has 256 values and four are used.

**Disposition: open.** Add the fifth value and a scenario.

### 15. Neither the stability window nor the retry interval has a number

**Where:** `design.md`, throughout
**Found by:** firmware-engineer, with the arithmetic.

The heap is 6136 usable bytes; the slowest leak is `TaskLogger`'s 56 B/s giving ~110 s to
exhaustion, the fastest is `TaskHeartbeat`'s two 304 B messages per second giving ~10 s. So
the window must exceed ~110 s and five minutes is the smallest defensible round number. The
retry interval must exceed the window by several multiples, so 30 minutes. Both numbers, and
that derivation, belong in `design.md` — a number with a derivation is auditable, an
adjective is not.

**Disposition: open.**

### 16. Where the periodic duties live is unstated, and only one task exists in every configuration

**Where:** `tasks.md:71-73` (5.2), `:125-127` (8.2)
**Found by:** qa-engineer and firmware-engineer, independently.

"From the existing periodic path" is ambiguous between three tasks. `TaskLogger` does not
run in the reduced set or with no card, so if either the stability clear or the retry
countdown lands there, the board never clears its counter and never retries — the trap R4
exists to forbid. Only `TaskHeartbeat` runs in every configuration.

**Disposition: open.** Name `TaskHeartbeat` in both, and make "it starts in every
configuration" an explicit invariant.

### 17. Smaller findings, recorded

- **`stop_control` and the refresh window are unspecified** (firmware). The core's own config
  uses `WDT_STOP_CONTROL_ENABLE`, which stops the count in sleep — and the idle hook executes
  `WFI`, so the timeout would mean CPU-awake time, not wall time. Specify `DISABLE`, and
  specify `WDT_WINDOW_START_100` so a refresh a millisecond after the previous one is not a
  refresh error and an immediate reset.
- **R6 forbids what 8.1 does** (qa). `spec.md:149-150` says the change "SHALL NOT change the
  vehicle identity, the message set, or the stream rates"; 8.1 adds a `COMMAND_ACK` where
  `src/mavlink.cpp:160-168` replies to nothing. Narrow R6 to identity and rates.
- **The Impact does not admit the shape of the verification in figures** (qa). It says "not
  verifiable without the board" but never 17 of 17 scenarios, 15 needing hands, 21 of 42
  steps, and three clauses with no method.
- **`custom_mode` is fully spent with no plan for the next thing that needs a continuing
  field** (operations). It proposes `SYS_STATUS`'s standard sensor bitmap, which MAVProxy
  decodes without configuration and which would also solve finding 8's card-absence gap.
- **Keep `TaskMavlinkBatteryStatus` in the reduced set** (firmware). It reads A0 with no bus
  that can stall, and battery state is the likeliest explanation for a board that reboots
  repeatedly. Related: the reduced set removes only the two tasks that touch the card, so
  "reduced" is exactly "no SD" — a fault anywhere else leaves the board reduced and still
  resetting, with the log off. That belongs in Risks.
- **The cumulative threshold has no decay** (firmware). Ten resets in an hour and ten in a
  year are the same to it; on a multi-year mission a healthy board eventually goes reduced.
- **The idle task is now safety-critical and is the only stack with no evidence** (qa,
  firmware). `-D INCLUDE_xTaskGetIdleTaskHandle=1` plus one field; measured to cost no extra
  heap block, since `sizeof(Data)` 44 and 48 both round to 56.
- **Cross-change collision with `add-usb-dual-protocol`** (qa, systems). It deletes
  `[env:bench]` and removes `check_silence.py`; every board receipt here assumes
  `pio test -e bench` is the way onto the link. `design.md` names it in Open Questions; the
  proposal does not, and `openspec/config.yaml` requires the proposal to.
- **`ARCHITECTURE.md:401` still claims the HIL cases follow the `mavlink-link` scenarios**
  (qa). Commit `4acab29` corrected `CLAUDE.md` and the README and missed this third copy.
  **Verified here.**
- **Three `TODO.md` facts this change invalidates, and one entry it partly satisfies**
  (systems), with the five cross-reference sites task 9.4 leaves unnamed.
- **Task 9.3 misdescribes its target** (systems). The `configASSERT` sentence it wants to
  change is in `CLAUDE.md`, not in `openspec/config.yaml`. As written the step is a no-op.
- **Task 5.3's verification would pass whether or not the work was done** (qa): `pio run`
  succeeding and headroom being unchanged are equally true of a branch that starts the wrong
  four tasks. Firmware adds that headroom will *not* be unchanged — the WDT control block is
  16 bytes — and that with `--gc-sections` a stack array no branch references would be
  discarded, reporting *more* headroom, which reads as good news and is a deleted task.
- **`STATUSTEXT` formatting in a 128-word task** (firmware). There is no `printf` family in
  `src/` today and newlib-nano's `vsnprintf` costs a couple of hundred bytes of stack against
  `TaskHeartbeat`'s 512.
- **A sagging battery would be misdiagnosed as a firmware fault** (hardware). Voltage-monitor-0
  reset is disabled by the OFS1 option bytes, unreachable from the application, so between the
  LVD0 threshold and the fixed POR level the MCU runs erratically until the watchdog eventually
  fires — recorded as `WDTRF`, indistinguishable from a hang. LVD1 **is** configurable at
  runtime (`LVD1CR0/LVD1CR1` under `PRCR.PRC3`) and its reset sets a distinct `RSTSR0` flag, at
  no RAM or pin cost. On a solar/LiPo satellite this is the single most valuable addition
  available to this capability that nobody had asked for.
- **SEU-relevant reset causes are available and unused** (hardware): `RSTSR1` already
  distinguishes SRAM parity, ECC, stack-pointer-monitor and bus-error resets, decodable from
  the same register read the change already performs. Related: SRAM parity errors are
  configured to raise an NMI by default with no handler installed, which hangs the board today
  independent of this change — worth its own `TODO.md` entry, separate from this one.
- **The MAVLink frame-size figures this project has quoted are wrong, corrected here**
  (hardware): `ARCHITECTURE.md:122` says a 68-byte `BATTERY_STATUS` stalls ~11.8 ms. Measured
  from `src/mavlink.cpp:97-115`'s packing and MAVLink 2's trailing-zero trim
  (`mavlink_helpers.h:115-121,245`): wire payload is 36 bytes, frame 48 bytes, **8.33 ms** at
  57600 baud — not 56/68 and not 11.8 ms. `proposal.md:26`'s "the other 511 are untouched"
  should read 508, per finding 4. Both are corrections to figures already in the tree, not to
  this change's own claims, and belong in the same commit as 9.1's other `ARCHITECTURE.md`
  edits.

## Disagreements

**1. Does opening the watchdog start it? Resolved.** The test plan's audit (a)4 assumed all
of `setup()` must fit inside `WDT_TIMEOUT_MS`; the firmware engineer said the counter does not
start until the first refresh, so a hang in `setup()` produces no reset at all. The hardware
engineer's later run reached the register semantics this review initially lacked and did not
contradict the firmware engineer's reading, so the disagreement resolves in the firmware
engineer's favour: the fix is to refresh at each phase-marker write (finding 6).

**2. How to fix the `NULL` handle hazard.** The test plan proposed an invariant on which
tasks may start together; the firmware engineer argues that is the wrong shape and the fix
belongs in `src/logger.cpp`. I judge the firmware engineer right, for the reason given in
finding 12: an invariant checkable by nothing, coupling two distant modules, that forbids a
configuration the change should want later.

**3. My own error, caught by two reviewers.** I stated earlier in this session that
`BOOT_DOUBLE_TAP_DATA` is `VBTBKR[0]`, meaning byte 0. It is a 32-bit access at byte 0. The
qa and firmware engineers found it independently; both are right and I was wrong, and the
error had already reached four documents.

## Verdict

**Not ready to apply, and further from ready than this document first concluded.** This
review was closed once with five findings pending and the hardware engineer's angle missing;
that run then completed and is folded in above. It confirmed one blocker was worse than
recorded (finding 1's original fix does not work either — see below), and added four more
that nobody else could have raised: an unverified but serious risk to the bootloader's own
recovery window (3b), a reset cause the register set cannot express (3c), a second path by
which the auto-retry defeats itself (3d is independent of 3/4 — uninitialised backup state
rather than the counter-accounting rule), and a variant of the deliberate-reset trap that
generalises finding 4 to every `NVIC_SystemReset()` the firmware performs, not only the two
named in it.

Ten findings now block apply: the upload path (1, revised), the bootloader's shared backup
registers (2), the possible DFU-arming interaction (3b, unverified), the missing
external-reset flag (3c), backup state with no validity marker (3d), the unreachable
auto-retry (3), deliberate resets counting as faults (4 and 3c together), the two falsified
archived capabilities (8), and the missing row-to-task chain (11). Findings 2, 3, 3b, 3c, 3d
and 4 are not six separate problems — they are six symptoms of one gap: this change reads and
writes shared MCU state (`VBTBKR`, `RSTSR0/1/2`) without first establishing what already
reads or writes it, at boot or in the bootloader that runs before any of this firmware's
code. That should be settled once, as a single pass over `include/Recovery.h` and task
section 1, before the individual fixes are written.

Finding 1 remains the most serious defect this project has produced, and it changed shape
rather than resolved: the fix originally adopted here — add the upload-guard flag test to the
overridden idle hook — does not work either, because relying on watchdog underflow to reach
the bootloader cannot fit under `dfu-util`'s 1000 ms detach timeout at any expressible
watchdog period. The fix now adopted jumps to the bootloader directly from the flag, which
removes the timing dependency entirely.

Nothing in this review has been applied to the artifacts yet. That is deliberate and not an
oversight: the fixes are substantive — new state in the layout, two MODIFIED deltas, a
reworked exit rule, a different watchdog integration point, a register-ownership audit before
any of section 1 is written — and belong in a revision pass, not in the margins of a review.
This document is the input to that pass.

**What a reader should know before starting.** The board is in DFU and has not been flashed.
17 of 17 scenarios need it and 15 need a person at it, so nothing here is demonstrable today
whatever is decided. All six reviewer roles reported; none is missing. One finding (3b) rests
on a disassembly of `bootloaders/UNO_R4/dfu_minima.hex` that could not be independently
reproduced in this session — it is recorded as an unverified hardware claim, in the shape this
role exists to produce, not as an established fact, and settling it is the first task in any
revision.

**This review read the verification steps and did not run them.** No suite was executed
against the board and the board was not touched. Three reviewers built the tree —
`pio run`, `pio check` — and two disassembled shipped binaries: `libfsp.a` by two reviewers
independently, and the Minima bootloader image by one. Those are inspections of the
toolchain's output, not exercises of the firmware. A step can trace to the right requirement,
name the right method and read correctly, and still not fire. Treat these as reviewed, not
exercised: the first time any of them runs is during apply.

## Note on the cost experiment

Three agents ran on opus and three on sonnet, to test whether model tier could reduce the
cost of a six-way review. **It does not.** The most expensive run was `systems-engineer` on
sonnet at 140 446 tokens, above `qa-engineer` on opus at 112 452. Cost tracked tool uses,
which is to say how much each agent chose to read: 12 tools cost ~60 k and 68 tools cost
~150 k, regardless of model. The lever is the scope each agent is given, not the model it
runs on. Recorded here so the experiment is not repeated.
