## Context

See `proposal.md` — Why. What follows is only what shapes the approach, and the three
hardware facts the whole design rests on were checked against the tree rather than
assumed.

**`RSTSR0/1/2` arrive intact.** `R_SYSTEM->RSTSR0` carries the power-on and low-voltage
flags, `RSTSR1` the independent-watchdog, watchdog and software-reset flags, `RSTSR2` the
cold/warm start flag. Nothing in `variants/MINIMA` or in the Arduino core reads or writes
them, so `setup()` is the first code to see them. They accumulate until cleared, which is
why clearing them is part of reading them.

**`VBTBKR` has 512 bytes and four are taken.** `cores/arduino/boot.h` defines
`BOOT_DOUBLE_TAP_DATA` as a 32-bit access at `&R_SYSTEM->VBTBKR[0]` — `VBTBKR` is
`__IOM uint8_t VBTBKR[512]`, so the double-tap magic occupies bytes `[0..3]`, not byte 0
alone. This change's own layout starts at `[4]`; 508 bytes remain free, not 511 — an
error that reached the proposal, this file, two tasks and a `TODO.md` entry before two
reviewers caught it independently (`review.md` finding 2). `#ifdef NO_BACKUP_REGISTERS`
is defined nowhere in the tree, so the real backup registers are in use, not the RAM
fallback at `0x20007FF0`. Writing them needs the `PRCR` unlock the bootloader path
already demonstrates, and that unlock races the same unlock/write/lock sequence
performed **from the USB interrupt** (`SerialUSB.cpp:164-166`) — an ISR landing between
this firmware's unlock and its store makes the store a silent no-op. The write helpers
(task 1.2) make the critical section their own property, so no call site has to
remember it (`review.md` finding 7). They are peripheral registers: they cost no RAM and
they survive a reset by construction. What they do *not* obviously survive is losing
VCC, which needs the VBATT domain powered — see Risks.

**The idle hook is weak and already enabled.** `configUSE_IDLE_HOOK` is 1 and
`portable/FSP/port.c` provides `__attribute__((weak)) vApplicationIdleHook()`. Overriding
it in `src/` is the intended extension point.

**The watchdog is not ours alone.** `cores/arduino/usb/SerialUSB.cpp`'s
`tud_dfu_runtime_reboot_to_dfu_cb()` opens the WDT itself as part of the 1200-baud upload
path, sets `is_watchdog_reset_in_progress_for_upload`, and handles
`FSP_ERR_ALREADY_OPEN` by spinning deliberately — the comment says it expects the
application to be kicking it. `BSP_CFG_PARAM_CHECKING_ENABLE` is `0`
(`bsp_cfg.h:28`), so the `FSP_ERROR_RETURN` that would produce that code is compiled
out in practice, and the callback returns instead of spinning (`review.md` finding 1).
That shapes the DFU-trigger decision below.

**The backup-register layout is established once, not field by field.** Findings 2, 3,
3b, 3c, 3d and 4 in `review.md` are six symptoms of one gap: reading and writing shared
MCU state (`VBTBKR`, `RSTSR0/1/2`) without first fixing, in one place, who owns which
byte and which flag. This table is that audit, and `include/Recovery.h` states it as one
block:

| Bytes | Owner | Content |
|---|---|---|
| `[0..3]` | the bootloader | the double-tap magic. Never read or written by this firmware. |
| `[4..5]` | this firmware | a validity magic byte and a checksum byte over `[6..11]`. On mismatch, `[6..11]` is treated as all-zero and the reset reason additionally records "backup state invalid" rather than trusting an undefined block (`review.md` finding 3d). |
| `[6]` | this firmware | the consecutive-unstable-boot counter |
| `[7]` | this firmware | the cumulative-reset counter |
| `[8]` | this firmware | the cumulative-count snapshot, taken on entering the reduced configuration (`review.md` finding 3) |
| `[9]` | this firmware | the phase marker |
| `[10]` | this firmware | the reset reason: power-on, watchdog, software, external/unknown (inferred), low-voltage, or backup-state-invalid |
| `[11]` | this firmware | the deliberate-reset marker: none, commanded, or retry, consumed by the next boot (`review.md` finding 4) |

Every write to `[4..11]` goes through the `PRCR`-unlocked helpers of task 1.2, which own
the critical section described above so no call site has to remember it. Both counters
read as 0 when the checksum does not match (finding 3d's validity check), but are
**not** clamped to their thresholds otherwise: implementing the snapshot comparison
below surfaced that clamping the stored value, as first proposed, would make "the
retry's own reset" and "a new fault while already at the cap" read identically once
the raw count reaches the threshold, defeating the mechanism it is there to support.
A threshold check compares with `>=` regardless of how far past it the count is; the
heartbeat clamps to the threshold only when packing the display value (`src/mavlink.cpp`).
`RSTSR0.PORF`'s possible interaction with the bootloader's own DFU-arming window
(`review.md` finding 3b, see below) is a read-only question about a register this
firmware does not write, independent of this layout — it gates how task 2.1 handles
`RSTSR0`, not what lives in `VBTBKR`.

## Goals / Non-Goals

**Goals:**

- Turn a hang into a reset, a reset into a known cause, and a repeated cause into a
  different boot.
- Keep the board reachable and commandable in the reduced configuration, including when
  the RTC and the card are both gone.
- Put the state in the heartbeat, so a ground station that connects at any moment learns
  it without asking.
- Make a halt in `setup()` diagnosable after the fact, which today it is not.

**Non-Goals:**

- Parking the board in DFU after repeated failures. It is tempting — the bootloader
  reads `VBTBKR[0]` and `goBootloader()` shows how — and it is exactly wrong in flight,
  where DFU is indistinguishable from dead. The bench wants it and the satellite does
  not, so it stays out until there is a reason to split the two.
- Per-task liveness bookkeeping. The idle hook detects that *something* stopped yielding,
  not which task. Naming the task needs shared state in every task body; the phase marker
  and the fault hooks cover the cases that matter for now.
- Recovering the currently bricked board. That is `TODO.md`'s own entry and needs hands.
- Changing the memory model, the queue protocol or the stream rates.

## Decisions

### The watchdog is refreshed from the idle hook

`vApplicationIdleHook()` runs only when every other task is blocked or delayed. A task
that spins without yielding, or that deadlocks at or above the idle priority, stops the
refresh by itself — no shared state, no per-task counters, nothing to keep in sync as
tasks are added.

**Alternative rejected:** refreshing from the lowest-priority task, which `TODO.md`'s
watchdog entry offers. It detects starvation of that task but keeps refreshing while
another task is wedged, which is the more likely failure.

**Alternative rejected:** each task marking a flag and one task refreshing when all are
set. It detects more — it names the task — at the cost of shared mutable state touched by
every task body, in a firmware whose freedom from mutexes rests on single ownership. Not
worth it for the first version.

**What this does not catch, and it is worth being blunt about it.** The idle hook
detects *starvation* — something is running and will not stop. It cannot detect
*silence*, because a firmware in which every task is legitimately blocked looks
identical to an idle one: the idle task runs, the refresh happens, and the watchdog is
satisfied. If the producers into `serialWriteQueue` all stopped, `TaskSerialWrite` would
wait on `xQueueReceive` for ever, `TaskMavlink` likewise, `TaskSerialRead` would poll and
find nothing, and the board would be mute while this mechanism reported it healthy.

Closing that needs a second gate: refresh only when a frame has recently left the link,
counted after `LINK_SERIAL.write()` in `src/serial.cpp` where returning means the bytes
are on the wire. It is cheap — one 32-bit word, one writer, one reader — but it is a
different mechanism with a different failure mode, and it drags in a change to
`TaskMavlink`'s `portMAX_DELAY`, which is a task body. Deliberately out of scope here and
written up in `TODO.md` instead, so this change stays one mechanism that can be judged on
its own.

The practical consequence to hold in mind: **this watchdog proves that the firmware is
running, not that the satellite is working.**

**Constraint to respect:** the port's weak hook does `__disable_irq()`,
`vTaskSuspendAll()`, `rm_freertos_port_sleep_preserving_lpm(1)`, then `__enable_irq()`
and `xTaskResumeAll()`. The override must reproduce that, not replace it: dropping the
sleep costs low-power idle permanently, which on a solar-powered satellite is a real
loss. The refresh goes inside, before the sleep.

### The DFU trigger jumps to the bootloader directly; it is not a watchdog side effect

The design originally relied on the watchdog underflowing during the core's DFU
callback: no task and no idle hook runs during that callback, so the refresh stops, the
WDT fires, and the reset lands in the bootloader because the magic was already written.
**This does not work, and cannot be made to work by tuning the timeout.**
`USB.cpp:145`'s `TUD_DFU_RT_DESCRIPTOR` sets `wDetachTimeOut` to 1000 ms; the shortest
timeout the RA4M1 WDT can express is 174.8 ms, but the timeout must also clear
`TaskSdWrite`'s worst case (up to 5.592 s, see below), and no single value serves both.
`dfu-util` gives up long before an underflow tuned for the SD card would ever fire
(`review.md` finding 1, the most serious defect this project has produced).

The core already exposes what the fix needs. `is_watchdog_reset_in_progress_for_upload`
(`SerialUSB.cpp:160`) is set by the DFU callback for exactly this, and the port's own
weak idle hook already tests it (`port.c:1482-1484`) — this change's override must not
drop that test. The overridden `vApplicationIdleHook()` checks the flag **before**
refreshing and, when set, calls `goBootloader()` (`cores/arduino/boot.h`) directly
instead of refreshing:

```c
extern bool is_watchdog_reset_in_progress_for_upload;
if (is_watchdog_reset_in_progress_for_upload) goBootloader();
R_WDT_Refresh(&wdtCtrl);
```

`goBootloader()` rewrites the double-tap magic and calls `NVIC_SystemReset()` directly,
so the jump takes microseconds and has no dependency on any watchdog period. This also
means enabling the watchdog no longer makes `pio run -t upload` slower — it should
become *faster* than the core's unmodified ~175 ms path, since the reset fires as soon
as the idle hook next runs rather than waiting on an underflow. This retires the upload
measurement as originally scoped in task 4.5: what is left to measure is that upload
still works, not how much slower it got.

### The timeout is a build flag, sized for `TaskSdWrite` alone

With the upload path no longer coupled to it, `WDT_TIMEOUT_MS` answers one question:
how long `TaskSdWrite`'s worst case — the ring rollover at
`lib/SdData/SdData.cpp:56-68`, which can delete a file of up to 1 GiB inside one
SPI-blocking call — is allowed to take. `TaskSdWrite` runs at `PRIORITY_LOWEST` with
`configUSE_TIME_SLICING` off, so while it spins the idle task does not run and the
watchdog is not refreshed: every ordinary `sdData.write()` must complete inside the
timeout. The RA4M1 WDT's register-start counter maxes out at 134 217 728 PCLKB cycles;
with `BSP_CFG_ICLK_DIV` at `/1` and `BSP_CFG_PCLKB_DIV` at `/2`, PCLKB is 24 MHz, so the
ceiling is **5.592 s** (`review.md` finding 5). The value is also discrete — seven
timeouts by ten dividers — so a millisecond figure must round to one the hardware can
express. It stays `-D WDT_TIMEOUT_MS` in `platformio.ini` so flight and bench can
differ, but both are bound by the same 5.592 s ceiling. If task 4.6's measurement shows
the rollover does not fit under it, the fix is a smaller ring file, not a larger flag.

### The reset reason comes from the hardware; the counters supply the memory

Reading `RSTSR0/1/2` answers "why did I restart", with one gap and one addition the
review surfaced:

- **A RES-pin reset sets none of the three registers.** `RSTSR0` carries only `PORF` and
  the LVD/deep-standby flags, `RSTSR1` the independent-watchdog, watchdog, software-reset
  and bus/parity/stack-monitor flags, `RSTSR2` only the cold/warm-start flag
  (`RSTSR2.CWSF`, which is *set by software*, not cleared — task 2.1 must not clear it
  the way it clears the other two registers). An external reset is therefore inferred by
  elimination, and only once every other flag is known-cleared. The reason is five
  values, not four: power-on, watchdog, software, external/unknown (inferred), and
  low-voltage (`review.md` findings 3c and 14).
- **A low-voltage reset is its own reason, not folded into power-on.** `RSTSR0` carries
  the LVD flags alongside `PORF`, as this file already noted in Context. On a satellite
  running from one LiPo cell with a naive charge estimate, a sagging battery is the cause
  most worth telling apart from an ordinary power-on, and a `uint8_t` reason field has
  252 unused values to spend on it (`review.md` finding 14).
- **`RSTSR0.PORF`'s clear behaviour is now settled against the RA4M1's own register
  definition, resolving finding 3b.** `review.md` finding 3b flagged, from an
  unreproduced disassembly of the bootloader, a risk that clearing `PORF` might
  interact with the bootloader's own DFU-arming window. The RA4M1's CMSIS register
  header (`R7FA4M1AB.h`, generated from the same data as the hardware manual)
  documents `PORF` as: "Power-On Reset Detect Flag. NOTE: Writable only to clear the
  flag. Confirm the value is 1 and then write 0" — the identical idiom already used
  for `LVD0RF`/`LVD1RF`/`LVD2RF`/`DPSRSTF` in the same register and for every flag in
  `RSTSR1`. No other automatic clear condition is documented. Because `PORF` is set
  to 1 only by the power-on-reset circuit itself, a genuine power-on always re-arms it
  to 1 regardless of what any earlier boot's software did; clearing it after reading,
  the same as every other flag, cannot cause a real power-on to be missed by whatever
  reads it next (this firmware or the bootloader), and only prevents the bit from
  staying latched at 1 forever after the first power-on the board ever experiences.
  Task 2.1 clears `RSTSR0` and `RSTSR1` uniformly, bit by bit, using this confirm-1-
  then-write-0 idiom.

Reading the reason does not answer "is this happening repeatedly", which is the question
that selects a configuration. Hence two counters in `VBTBKR`, and they are not
redundant:

| | Cleared by | Catches |
|---|---|---|
| Consecutive unstable boots | reaching stability, or a deliberate reset | a fault at or near boot |
| Cumulative resets | a ground command only | a fault that appears after stability |

The second exists because of this firmware specifically. `CLAUDE.md` describes a heap
leak as fatal within minutes: with only the consecutive counter and a stability window of
seconds, every boot would be declared stable, the counter would reset, and the board
would reboot forever without ever choosing a reduced configuration.

**Decision on the stability window:** longer than the fault it must outlive, which for
the leak case is minutes rather than seconds. A window that is too long has the opposite
failure — a board crashing just inside it never counts as unstable — and that is exactly
what the cumulative counter covers. The two together tolerate a badly chosen window;
either alone does not.

**Decision on who advances the consecutive counter, and the deliberate-reset carve-out
it needs:** watchdog and software resets advance it; power-on, external and low-voltage
resets do not — someone is present for those three, and erasing their evidence would be
worse than not counting them, so they are recorded in the cumulative count and in the
heartbeat without moving the trigger. But not every software reset is evidence of a
fault: both exits from the reduced configuration — the ground command (task 8.1) and the
automatic retry (task 8.2) — are software resets, and without a marker, three commanded
reboots would put the board into the reduced configuration by themselves, and the
auto-retry's own reset would count as evidence of the fault it exists to test for
(`review.md` finding 4). The **deliberate-reset marker** (layout byte `[11]`) is written
immediately before every `NVIC_SystemReset()` this firmware itself performs — today
that means tasks 8.1 and 8.2 only, but any future call site this project adds must set
it the same way or be recorded as a fault by default, which is the safe default for an
unmarked reset. The next boot consumes the marker: neither *commanded* nor *retry*
advances the consecutive counter.

**Decision on the cumulative-count trap, and why it needs a snapshot, not a rule
change:** the requirement that the firmware "leave the reduced configuration on its own
after a long interval" is unsatisfiable against a raw cumulative threshold, for exactly
the case it exists for. Walk it: no command can arrive by premise, so the auto-retry
resets the board; that reset is itself recorded in the cumulative count; the next boot
re-evaluates the raw threshold, finds it still exceeded, and returns to reduced before
emitting a heartbeat — deepening the trap on every attempt (`review.md` finding 3). The
fix is state: on **entering or remaining in** the reduced configuration, the cumulative
count is snapshotted into layout byte `[8]`. The retry's own reset is itself a software
reset and advances the cumulative count by one, same as any other — that increment is
expected, not a fault, so the boot decision that follows an automatic retry compares
the *current* cumulative count against *snapshot + 1*, not against the raw threshold:
the retry succeeds unless the cumulative count grew by more than its own contribution,
i.e. unless a further fault occurred during the wait or the retry attempt itself. The
snapshot is cleared, along with both counters, on the ground command and on any boot
that reaches stability normally.

### The phase marker is the cheapest thing here and the most useful

One byte, written at each milestone of `setup()`. A reset at the clock phase names
`systemTime.begin()`; at the card phase, `SD.begin(9)`. It costs a single store per
milestone and it is what turns today's silent halt into something the ground can read.
Both fault hooks in `src/hooks.cpp` write a marker before they blink, so a watchdog reset
that follows a stack overflow or an allocation failure is attributable to it rather than
looking like a generic hang.

### The `configASSERT` policy is replaced, not softened

`CLAUDE.md` requires this to be an explicit decision, so it is stated as one. The
question it turns on is not "is the RTC important" but "what does the firmware need in
order to be reachable", and the answer is: the link, and nothing else.

- **No RTC** → run on ticks since boot. `src/mavlink.cpp` already accepts an inbound
  `SYSTEM_TIME` and sets the clock, so the ground supplies what the DS1307 would have.
  Timestamps before that are relative and must be reported as such rather than as
  epoch zero.
- **No card** → `TaskLogger` and `TaskSdWrite` do not start. Nothing else reads the card.
- **No queue, no task** → impossible to observe since `use-static-allocation`:
  `xQueueCreateStatic` and `xTaskCreateStatic` cannot fail for want of memory, and the
  build refuses a firmware that does not fit. Those two asserts stay as argument checks
  and are no longer part of the boot policy.

The original policy's reasoning — that a CubeSat with no clock or no log produces data
that cannot be trusted afterwards — is right about the *data* and wrong about the
*mission*. A board that halts produces no data at all, and cannot be told anything.

### The heartbeat carries the diagnosis in a field already being wasted

`custom_mode` is a `uint32_t` that this firmware sends as zero on every heartbeat. It
becomes the reset reason, the phase reached, and both counters, one byte each. Cost: zero
bytes on the wire, once per second, with no request from the ground. `system_status`
becomes `MAV_STATE_CRITICAL` when reduced and `base_mode` drops
`MAV_MODE_FLAG_AUTO_ENABLED`, both of which MAVProxy surfaces without configuration.

The identity triple does not move, but that is not the same as `mavlink-link` being
untouched. Two archived scenarios are falsified by this change: `mavlink-link`'s *Board
powered with nothing attached* promises "all tasks reach their steady-state cadence" and
"housekeeping records continue... at 1 Hz", both false in the reduced configuration and
false with no card; `memory-budget`'s *A ground station sees no difference* requires the
full message set at the rates `mavlink-link` defines, which the reduced configuration
drops `BATTERY_STATUS` from. Both need a MODIFIED delta qualifying the scenario to the
normal configuration, cross-referenced to `fault-recovery` (`review.md` finding 8) — see
`proposal.md`'s Capabilities section.

**Alternative rejected:** `STATUSTEXT` alone. It is a one-shot at boot, so a ground
station that connects afterwards sees a normal-looking vehicle. The `STATUSTEXT` stays,
but as the human-readable addition rather than the mechanism.

### Ownership

- **`src/main.cpp`** owns the boot decision, as the composition root: it reads the
  reason and the counters, writes the phase marker, and chooses the task set. This is
  the file the change makes conditional, and the concentration of risk is deliberate —
  one place decides, everything else stays ignorant of which configuration it is in.
- **`src/hooks.cpp`** owns the kernel hooks, and so owns the watchdog refresh and the
  phase writes from the two fault hooks. It already owns the fault indicator.
- **A new `include/Recovery.h`** owns the backup-register layout, the phase enumeration
  and the reset-reason encoding — declarations only, no state. `src/main.cpp` writes them
  and `src/mavlink.cpp` reads them for the heartbeat, so neither can own the definition
  without the other reaching into it. This mirrors how `include/Link.h` and
  `include/Data.h` are shared without owning behaviour.
- **`src/mavlink.cpp`** keeps owning every outbound message, including the new heartbeat
  fields and the boot `STATUSTEXT`, and the `COMMAND_LONG` handling that already exists.
- The clocks stay with `lib/SystemTime`, the card with `src/sdwrite.cpp`, the ADC with
  `lib/Battery`. The change reaches none of them; what changes is whether their tasks are
  started.

## Risks / Trade-offs

- **`VBTBKR` may not survive losing VCC** → It needs the VBATT domain powered, and how
  the Minima wires that is not established. Surviving a *reset* is certain, because the
  bootloader's double-tap depends on it, and that is all the counters require. A design
  that needed them to survive a full power cycle would be resting on something
  unverified; this one does not, and arguably should not want to — a cold start after the
  battery recovers is a reasonable fresh chance. **Must be checked on the board before
  the auto-retry interval is trusted.**
- **`src/main.cpp` becomes conditional, and it is the one file with no test** → The
  concentration is deliberate but it is still concentration. Every path has to be
  exercised on the board, and there are now four: normal, no RTC, no card, reduced.
- **The stability window is a guess** → Mitigated by the cumulative counter rather than
  by getting the window right. Called out because the first flight will show whether the
  window or the threshold is wrong, and the SD log plus the heartbeat counters are what
  will show it.
- **`RSTSR0.PORF`'s clear behaviour was resolved from the RA4M1's own register
  definition, not from the board** → See the reset-reason decision above. The
  resolution rests on the CMSIS header's documented bit semantics, not on a
  board measurement; task 9.7's board work should still confirm no surprising
  DFU-arming behaviour is observed once the board is reachable again.
  (`review.md` finding 3b.)
- **Backup-register content has no validity marker until task 1.1 adds one** → An
  uninitialised block read as counters, or one corrupted by an SEU or a brownout mid-write,
  is indistinguishable from genuine fault history without the magic-plus-checksum check
  in the layout table above. (`review.md` finding 3d.)
- **A watchdog can mask a fault instead of exposing it** → A board that reboots quietly
  every few minutes and keeps transmitting looks healthier than one that hangs. The
  cumulative counter in the heartbeat is the defence: the resets are visible from the
  ground whether or not anything else is.
- **A mute-but-not-stuck firmware is invisible to this change** → See the decision above.
  The gap is real, it is named, and it has a backlog entry. It is not closed here.
- **A wedged card resets a board that is otherwise fine** → `TaskSdWrite` sits at
  `PRIORITY_LOWEST`, which is the idle priority, and `SPI.cpp` busy-waits with no timeout.
  A card that stalls starves only the idle task: every other task keeps running and the
  link keeps transmitting, yet the watchdog fires. Idle starvation cannot tell "one
  low-priority task is wedged" from "everything is wedged". What makes it tolerable is
  the rest of this change — three such resets select the reduced configuration, which
  does not start `TaskSdWrite`, and the cause stops recurring. The first two resets are
  disruptive and there is no way around them short of making the SPI transfer yield.
- **Enabling the watchdog changes `pio run -t upload`** → It still works, and the
  mechanism is understood, but it becomes slower by the timeout. Must be measured, and it
  is a step that needs the board.
- **Nothing here is verifiable by building it** → More than in any previous change. Every
  requirement is about a reset, a register that survives one, or a task set chosen at
  boot. The board is unreachable today, and this change should be flashed only once it is
  recovered — and flashed first with a short timeout and a way back.

## Migration Plan

No persisted format and no on-disk state, but unlike previous changes this one leaves
state in hardware: the counters live in backup registers that survive reflashing. A
firmware flashed over a board that had counted two failures starts from two. So the first
step after flashing is to clear them, and a reverted firmware leaves them set with nobody
reading them — harmless, but worth knowing when the next version reads them again.

Rollback is reverting the commit. The watchdog stops being opened, so the upload latency
returns to normal, and the RTC and SD asserts halt again.

Order: after `TODO.md`'s *Recover the bricked board*, and not before. Flash with a short
`WDT_TIMEOUT_MS` first, confirm the board comes up and can be reflashed, then raise it to
the value `TaskSdWrite` needs.

## Open Questions

- Whether the bench build should park in DFU after repeated failures, using the same
  counters and `goBootloader()`. It would have saved the currently bricked board. It is
  deliberately out of scope here because it needs the flight and bench policies to
  diverge, which is a decision of its own — and `bench` is itself slated for deletion by
  `add-usb-dual-protocol`.
- Whether the reduced configuration should eventually be able to report *which* task
  stopped yielding, which needs the per-task liveness scheme rejected above.
