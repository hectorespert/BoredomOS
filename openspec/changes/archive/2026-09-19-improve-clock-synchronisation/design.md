## Context

See proposal.md — Why. What shapes the approach beyond that:

- `lib/SystemTime` owns both clocks and is the only file that touches either
  (`ARCHITECTURE.md` §6). That is what makes the absence of mutexes safe, so every
  decision below has to leave it true or make it more true.
- The RA4M1's RTC exposes a **sub-second counter register**, `R64CNT`, and a periodic
  interrupt selectable up to 256 Hz. The board does not expose the DS1307's `SQW`
  output, so the external-interrupt route to sub-second phase is unavailable; these two
  internal facilities are what remain.
- The Arduino core drives the internal RTC from `LOCO`, an on-chip RC oscillator
  (`RTC.cpp`'s `#ifndef RTC_CLOCK_SOURCE` / `RTC_CLOCK_SOURCE_LOCO`), which is almost
  certainly why the DS1307 exists in this design at all.
- `mavlinkPack()` runs in `TaskLinkWrite`'s context, not in `TaskMavlink`'s: `src/link.cpp`
  dequeues a `LinkMsg` and then calls it. Anything `mavlinkPack()` reads is read after a
  queue hop of unbounded latency.
- Both reference autopilots answer `TIMESYNC` with monotonic time since boot in
  nanoseconds, timestamped at reception — ArduPilot's `timesync_receive_timestamp_ns()`
  and PX4's `rsync.tc1 = now * 1000ULL` off `hrt_absolute_time()`. The dialect itself
  specifies no time base for the field, so the convention comes from implementations,
  not from the protocol.

## Goals / Non-Goals

**Goals:**

- Add no mutex, no task, no queue, and no shared mutable state that two tasks write.
- Leave the no-clock configuration as the same code path as the normal one, not a
  branch, so that `specs/fault-recovery/spec.md`'s degraded requirement is satisfied by
  construction rather than by a parallel implementation.
- Leave the planned DataFlash log able to reuse this without extending it (see
  proposal.md — Impact).

**Non-Goals:**

- Changing the internal RTC's clock source. Measured, recorded, not acted on here.
- Any precision claim about the *absolute* accuracy of the reported time. This change
  fixes resolution, provenance and time base; how far `LOCO` drifts is a measurement
  this change produces, not a number it improves.
- Sub-second precision in the SD log. `Data.unixtime` stays whole seconds.

## Decisions

### Sub-second resolution comes from `R64CNT`, not from the tick

`getUnixTimeUsec()` becomes `seconds * 10^6 + (R64CNT & 0x7F) * 15625 / 2`.

**Seven bits, 1/128 s per count** — and the register's name misleads about exactly this.
Its bits are named for the frequency at which each one *toggles*: bit 0 is F64HZ, bit 5
is F2HZ, bit 6 is F1HZ. A counter whose least significant bit toggles at 64 Hz increments
at 128 Hz, and F1HZ toggling at 1 Hz means one second spans bits 0..6, i.e. 128 counts.
A first implementation here masked six bits and 15625 µs per count, taking the name at
face value; the fraction then wrapped twice a second and sent the reported time back by
almost a second, halfway through every second. The Unity case that hammers the accessor
found it on its first run and nothing sampling at 1 Hz could have.

| Option | Resolution | State to maintain | Rejected because |
|---|---|---|---|
| `R64CNT` register read | 1/128 s | none | **chosen** |
| RTC periodic IRQ at 256 Hz | 1/256 s | a counter in an ISR | an interrupt every 3.9 ms, and an ISR, to buy resolution nothing needs |
| Poll for the second to roll over, anchor to ticks | 1 tick, then drifts | anchor, re-armed periodically | two independent oscillators to keep in phase, and ~1 s of a task at boot |
| DS1307 `SQW` on an interrupt pin | exact | an ISR | the pin is not reachable on this board |

The decisive property is not the resolution, it is that `R64CNT` and the seconds
register are driven by **the same divider chain**, so the fraction is phase-locked to
the second it accompanies by construction. Nothing has to be kept in agreement, and
because the read is stateless it is callable from any task without a mutex — which the
tick-anchor option is not, since its anchor is mutable state with several readers.

**The read needs carry protection.** A read can land across the increment and pair a
stale second with a fresh fraction, which would make the reported time jump backwards
by a second — exactly the fault this change exists to remove. `lib/SystemTime` reads
seconds, then `R64CNT`, then seconds again, and retries while the second moved. The
retry is bounded: the second moves once per 128 counts, so one further attempt suffices.

### Monotonic time is a subtraction against a boot epoch, not an accumulator

`lib/SystemTime` latches the wall time once at boot into a `uint32_t _bootEpoch`, and
elapsed time is `wall − _bootEpoch`.

**The epoch is stored as `uint32_t`, not as `time_t`.** `time_t` on this toolchain is
**8 bytes, aligned to 8** — measured, not assumed — so a `time_t` store is two word
stores and a reader can tear across them. UNIX seconds fit in an unsigned 32-bit value
until 2106, and the plausibility floor below guarantees the stored value is far above
zero, so the narrowing is safe by construction rather than by hope. This keeps the
single-aligned-word property the rest of this decision depends on.

Rejected: a 64-bit microsecond counter extended in software from the 32-bit tick — which
is what the DataFlash-log backlog entry currently plans. It needs a wrap count, the wrap
count is mutable state, and it would be read by three tasks; a 64-bit load is not atomic
on a Cortex-M4, so a read landing on a wrap can return a value 49.7 days out. Making it
safe means a seqlock or a single owning task, both of which introduce a concurrency
primitive this firmware does not currently need anywhere. The subtraction needs none:
one 32-bit word, one writer, and no counter at all.

Also rejected: exposing only 32-bit millisecond deltas and never an absolute elapsed
figure. It would work, but `TIMESYNC` wants nanoseconds in 64 bits and the planned log
wants microseconds in 64 bits, so the absolute figure has to exist somewhere.

**The trade-off is resolution.** Elapsed time inherits the RTC's 1/128 s granularity
instead of the tick's 1 ms, and it inherits the RTC's drift rather than the tick's. For
`TIMESYNC` and for records at 1 Hz and below that is immaterial; it is recorded here so
that a future consumer needing 1 ms knows it is choosing a different base, not fixing a
bug.

### Accepting a time shifts the epoch by the same delta

`setUnixTime()` applies the correction to the clock **and** to `_bootEpoch`. Without
this, a clock set makes elapsed time jump by the size of the correction, which is
usually decades.

This is what lets this firmware accept a **backwards** correction, which ArduPilot
refuses outright (`// can't allow time to go backwards, ever`). ArduPilot's wall clock
and its monotonic timestamps are coupled, so a backwards step would corrupt its logs
and estimators. Here they are separate: the wall clock steps, elapsed time does not
move. And accepting backwards corrections is not optional for us — if the clock has
drifted forwards, a backwards step from the ground is the only way to fix it.

**Concurrency, stated precisely.** The clock write and the epoch update are two stores
and cannot be made one. A reader that lands between them would compute a wrong elapsed
value. That is safe today for a reason worth writing down rather than discovering:
**the only reader of elapsed time is `TaskMavlink`, which is also its only writer** —
`src/logger.cpp` reads wall-clock seconds, not elapsed time, and the log's elapsed field
is out of scope. Wall-clock reads are unaffected, being a stateless register read with
its own carry protection. When a second task starts reading elapsed time — which is
precisely what the DataFlash log change does — the pair update must be made indivisible,
with the scheduler suspended across it rather than with a mutex. `lib/SystemTime.h`
carries that as a comment on the accessor, so the obligation travels with the API.

### The source ladder, and why the internal clock is not on it

`lib/SystemTime` exposes an origin with four values, modelled on ArduPilot's
`AP_RTC::source_type` including its ordering — a time from the ground outranks the
hardware clock there, and does here:

```
  ground    (highest)  set from the ground this boot
  ds1307               seeded from the DS1307 by begin()
  survived             the internal RTC was already running with a plausible time
  none      (lowest)   nothing seeded it; the internal RTC started at zero
```

The RA4M1's internal RTC does **not** appear as a source, because it is not one: it is
the register that holds and advances the time, the counterpart of ArduPilot's
`rtc_shift`, which likewise has no entry in that enum. `SOURCE_HW` there means a
battery-backed clock read at boot — our DS1307. What gets ranked is whatever last wrote
into the internal RTC.

`survived` is the one case where the internal RTC does act as a source: after a restart
that left the board powered, it holds a time this boot did not put there.

**`ds1307` outranks `survived` for a coupled reason, not an aesthetic one.** With
`setUnixTime()` fixed to stop skipping the DS1307, every accepted ground time reaches the
DS1307, so the DS1307 can never hold worse information than the internal clock — and it
has a crystal where the internal runs on `LOCO`. If that propagation were dropped, this
ordering would have to invert. The two decisions are load-bearing on each other and a
later change must not simplify one without the other.

### The sanity floor does double duty

A fixed `2022-01-01T00:00:00Z`, after ArduPilot's `oldest_acceptable_date_us`, as a
compile-time constant. Rejected: deriving it from the build date, which is
`__DATE__`-shaped string parsing for no benefit, and makes the accepted range depend on
when the firmware happened to be compiled.

The same predicate decides whether a still-running internal RTC survived a restart with
something worth keeping. An internal RTC that was never set reads as 1970 and fails the
floor; one that was set reads plausibly and passes. **That is why a ground-set clock
survives a reset with nothing persisted** — no byte in `VBTBKR`, no extension of
`include/Recovery.h`'s checksum range, no new failure mode in the recovery block.

### `RTC.begin()`'s return value cannot be used as evidence

The core's `openRtc()` returns `true` from its failure branch as well as its success
branch, so `RTC.begin()` reports success even when `R_RTC_Open` failed. The origin is
therefore derived from `RTC.isRunning()` plus a `getTime()` that passes the floor, never
from `begin()`'s result. This is also why simply fixing the `_ds1307.begin() && RTC.begin()`
short-circuit is not sufficient on its own: the fixed expression would still be
uninformative.

`openRtc()` deliberately does not restart a clock that is already running
(`if(!isRtcRunning()) { R_RTC_ClockSourceSet(...) }`), and the core exposes
`isRunning()` and `setTimeIfNotRunning()`, so the `survived` path is supported by the
core rather than working against it.

### `tc1` is captured on receipt and carried through the queue

`TaskMavlink` reads elapsed time when it dequeues the `TIMESYNC` request and puts the
value in the `LinkMsg`; `mavlinkPack()` copies it instead of reading the clock.

This fixes two things at once. The **time base** is a convention requirement — both
reference autopilots use elapsed time since boot. The **capture instant** is accuracy:
the ground computes its offset from `tc1` against `ts1` plus half the round trip, so
`tc1` must be when the request arrived. Reading the clock inside `mavlinkPack()` puts
`TaskLinkWrite`'s scheduling and queue latency inside the number, where the ground
cannot distinguish it from clock offset.

`src/link.cpp` consequently loses its only indirect reach for the clock. Ownership gets
stricter, not looser.

The `timesync` variant of `LinkMsg` grows by 8 bytes, 16 to 24, against a union sized by
the 52-byte `statustext` variant, so `sizeof(LinkMsg)` is expected to be unchanged and
neither link queue's storage array moves. `include/LinkMsg.h`'s existing
`static_assert(sizeof(LinkMsg) <= 64)` is the check; a task confirms the figure against
a real build rather than trusting this paragraph.

We do not adopt ArduPilot's `receive_time_constraint_us()`, which back-calculates the
instant the frame entered the UART from its length and baud rate. At our message rates
the queue hop was the dominant error and capturing in `TaskMavlink` removes it; the
remaining per-byte transfer time is a refinement with its own failure modes.

### An unknown clock is reported as zero

`SYSTEM_TIME.time_unix_usec` carries `0` while the origin is `none`, after ArduPilot's
`send_system_time()` (`// may fail, leaving time_unix at 0`).

Rejected: the magnitude heuristic the dialect also documents and madflight relies on —
*"the receiving end can infer timestamp format ... by checking for the magnitude of the
number"* — which would have us put elapsed time in the UNIX field. It avoids a sentinel
but it means emitting a number that a ground station may render as a date in 1970, and
it is indistinguishable from a clock genuinely set to 1970. `0` says "unknown" and
cannot be mistaken for a reading.

Internally the firmware still runs on time measured from boot in that configuration,
which is what `specs/fault-recovery/spec.md` requires; the internal RTC simply starts at
zero. The requirement is about what the firmware operates on, the sentinel is about what
it claims to know.

### The boot report states whether a clock was found running, separately from the origin

The DS1307 on this board cannot be disconnected — not "was not disconnectable during
that session", which is how `add-degraded-mode` recorded it, but as a standing property
of the assembly. That has a consequence for this change specifically: `ds1307` outranks
`survived` in the ladder, so while the DS1307 answers, the selected origin is never
`survived` and **the ladder itself hides the evidence** that the internal RTC kept
running. The reduced configuration is no help either — `src/main.cpp` calls
`systemTime.begin()` unconditionally, so there is no configuration in which the DS1307
goes unread.

So the boot `STATUSTEXT` carries two facts, not one: the origin that was selected, and
whether the internal RTC was **already running with a plausible time** when `begin()`
started. The second is not a source and does not enter the ladder; it is an observation
about the state the previous boot left behind.

That single extra token is what makes the hardware assumption the `survived` source rests
on measurable at all on this board: command a restart, read the next boot's report, and
the answer is there with the DS1307 still attached and still winning. Without it the
assumption would be untestable here and the `survived` code path would be unexecuted
forever.

Rejected: a build flag or a ground command that makes the firmware ignore the DS1307 to
force the other paths. It is flight code carrying a switch whose only purpose is testing,
and a switch that can be wrong in flight, to buy coverage of a configuration this board
cannot enter anyway.

**The `none` origin remains unobservable on this board** and the requirement covering it
ships unexercised, disclosed in proposal.md — Impact. It is kept rather than dropped
because a DS1307 that cannot be unplugged on the bench can still fail in orbit, and that
failure is exactly the `none` case; dropping it would be optimising for the bench.

### A rejection emits nothing

A `STATUSTEXT` per rejected time would let a peer generate unbounded outbound traffic by
repeating an implausible value, saturating a write queue (depth 6, raised from 5 by this
change for the boot and origin-change texts) and starving the
periodic cadences that `specs/mavlink-link/spec.md` guarantees. The origin appears in the
boot report and a later change of origin emits one text; both are bounded by events the
firmware controls. A rejection is observable as the reported time not moving.

### The re-seed interval, and why it stops at first ground contact

While the origin is `ds1307` or `survived`, `TaskMavlink`'s schedule re-reads the DS1307
and re-seeds the internal clock, at an interval on the order of hours — the exact figure
is picked in tasks against the drift the board actually shows, not chosen here. Once a
ground time has been accepted this boot, the re-seed stops: the ladder says a
lower-ranked origin does not replace a higher-ranked one, and honouring that literally
keeps the ladder's meaning intact.

Considered and rejected: continuing to re-seed after ground contact on the grounds that
the DS1307 now holds the ground's time anyway. It is true only when the DS1307 write
succeeded, so it would need a read-back verification and an "unreliable DS1307" flag —
a new failure mode, to correct drift that the next ground contact corrects anyway.
Revisit if the measured `LOCO` drift turns out large enough to matter across the gap
between passes; that measurement is a task here.

The re-seed emits nothing, so it adds no producer to any queue and does not touch
`sdWriteQueue`'s or the link queues' depth arithmetic. It carries no spec requirement
because no ground station, operator or build can observe whether it happened.

### The sub-clock is measured, not selected

`platformio.ini` could define `RTC_CLOCK_SOURCE=RTC_CLOCK_SOURCE_SUBCLK`, and every
variant's BSP config declares `BSP_CLOCK_CFG_SUBCLOCK_POPULATED (1)`. Two things are
unverified: whether the Minima physically populates the crystal, and whether a
`build_flags` `-D` reaches the core's compilation unit at all. Selecting `SUBCLK`
without the crystal yields a **stopped** clock, which is a worse failure than a drifting
one, so this change measures the drift and records the finding, and a follow-up backlog
entry acts on it if the measurement supports it.

### Resource ownership

| Resource | Owner after this change |
|---|---|
| Internal RTC, including `R64CNT` | `lib/SystemTime` (unchanged) |
| DS1307 on I2C | `lib/SystemTime` (unchanged) |
| `_bootEpoch`, the origin | `lib/SystemTime`; written only via `setUnixTime()` / `begin()` |
| Inbound time validation, `tc1` capture, the re-seed schedule | `src/mavlink.cpp` |
| Packing a `LinkMsg` onto the wire | `src/mavlink.cpp`'s `mavlinkPack()`, called by `src/link.cpp` — and it no longer reads the clock |

No file gains access to a peripheral it did not already have, and `src/link.cpp` loses
one.

## Risks / Trade-offs

**The internal RTC may not keep running across a software reset** → the `survived`
origin and one `system-clock` requirement depend on it. It is in the battery-backed
domain and the core is written for the case, but that is the manual, not this board.
Measured first, before anything is built on it, using
`MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, which the firmware already answers. If it does not
survive, the requirement and that source value come out of this change before it lands
rather than shipping as a contract nothing satisfies.

**The epoch's stored width could drift** from the 32 bits that make its store a single
aligned word — this design originally assumed `time_t` was 4 bytes and it is 8, which is
why the type is now explicit. A `static_assert` on the stored member's size, and one
placing the plausibility floor below `UINT32_MAX`, turn a future regression into a build
failure rather than a torn read.

**`R64CNT` read without the retry** → timestamps that step backwards across a second
boundary, intermittently and rarely, which is the hardest possible failure to notice
from telemetry. The retry loop is small enough to review and gets a Unity case that
hammers the accessor and asserts monotonicity.

**Elapsed time gets a second reader before the pair update is made indivisible** → a
wrong elapsed value under preemption. Mitigated by putting the obligation in the header
next to the accessor, and by the DataFlash-log entry being re-pointed at this change,
so the next consumer meets the note before it writes code.

**`LOCO` drift may be bad enough that a clock is poor between ground contacts** → not a
regression (it is today's behaviour, merely unmeasured) but it may make the re-seed
decision wrong. The measurement is a task, and the decision above names the condition
under which it is revisited.

**`pio test -e libs` is destructive** — it replaces the firmware with the Unity binary
and deletes `data*.mpk` and `index.bin` from the card. Touching `lib/SystemTime` means
running it. Every task that does says so, and is followed by `pio run -t upload`.

## Migration Plan

None in the deployment sense — there is one board and one image. Two ordering
constraints inside the change:

1. The internal RTC must be started unconditionally before anything reads `R64CNT` or
   the origin, since neither exists on a clock that was never opened. That makes the
   `add-degraded-mode` 6.1 fix the first code task, not a cleanup at the end.
2. The reset-survival measurement comes before the `survived` source is implemented, so
   that a negative result costs a measurement rather than a rewrite.

Rollback is `git revert` plus `pio run -t upload`; nothing persists state that a
previous image would misread, since this change deliberately adds nothing to `VBTBKR`.

## Open Questions

- The re-seed interval. Deferrable: it changes one constant, no spec requirement and no
  task structure, and the drift measurement that informs it is already a task here.
