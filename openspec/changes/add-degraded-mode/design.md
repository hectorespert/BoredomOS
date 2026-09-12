## Context

See `proposal.md` — Why. What follows is only what shapes the approach, and the three
hardware facts the whole design rests on were checked against the tree rather than
assumed.

**`RSTSR0/1/2` arrive intact.** `R_SYSTEM->RSTSR0` carries the power-on and low-voltage
flags, `RSTSR1` the independent-watchdog, watchdog and software-reset flags, `RSTSR2` the
cold/warm start flag. Nothing in `variants/MINIMA` or in the Arduino core reads or writes
them, so `setup()` is the first code to see them. They accumulate until cleared, which is
why clearing them is part of reading them.

**`VBTBKR` has 512 bytes and one is taken.** `cores/arduino/boot.h` defines
`BOOT_DOUBLE_TAP_DATA` as `R_SYSTEM->VBTBKR[0]`, guarded by `#ifdef NO_BACKUP_REGISTERS`
which is defined nowhere in the tree — so the real backup registers are in use, not the
RAM fallback at `0x20007FF0`. Writing them needs the `PRCR` unlock the bootloader path
already demonstrates. They are peripheral registers: they cost no RAM and they survive a
reset by construction. What they do *not* obviously survive is losing VCC, which needs
the VBATT domain powered — see Risks.

**The idle hook is weak and already enabled.** `configUSE_IDLE_HOOK` is 1 and
`portable/FSP/port.c` provides `__attribute__((weak)) vApplicationIdleHook()`. Overriding
it in `src/` is the intended extension point.

**The watchdog is not ours alone.** `cores/arduino/usb/SerialUSB.cpp`'s
`tud_dfu_runtime_reboot_to_dfu_cb()` opens the WDT itself as part of the 1200-baud upload
path, and handles `FSP_ERR_ALREADY_OPEN` by spinning deliberately — the comment says it
expects the application to be kicking it. That shapes the timeout decision below.

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

### The timeout is a build flag, because it is also the upload latency

`TaskSdWrite` blocks on SPI and a card can take a surprisingly long time on a block
erase, so the timeout has to be generous — and the core's DFU path turns the
application's timeout into how long `pio run -t upload` waits before the board resets
into the bootloader. Those two pull in opposite directions, so the value is
`-D WDT_TIMEOUT_MS` in `platformio.ini` rather than a constant, and the flight and bench
environments can differ.

This also means the upload path keeps working with the watchdog enabled, for a reason
worth writing down: the DFU callback runs in the USB interrupt, no task and no idle hook
runs during it, so the refresh stops, the WDT fires, and the reset lands in the
bootloader because the magic was written first. It works *because* the refresh is not in
an interrupt.

### The reset reason comes from the hardware; the counters supply the memory

Reading `RSTSR` answers "why did I restart". It does not answer "is this happening
repeatedly", which is the question that selects a configuration. Hence two counters in
`VBTBKR`, and they are not redundant:

| | Cleared by | Catches |
|---|---|---|
| Consecutive unstable boots | reaching stability | a fault at or near boot |
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

**Decision on who advances the consecutive counter:** watchdog and software resets do;
power-on and external resets do not. Someone is present for the latter two, and erasing
their evidence would be worse than not counting them — so they are recorded in the
cumulative count and in the heartbeat, without moving the trigger.

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

The identity triple does not move, so `mavlink-link` is untouched.

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
