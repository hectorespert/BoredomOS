## Why

`lib/SystemTime` reports whole seconds: `getUnixTimeUsec()` and `getUnixTimeNsec()`
are `getUnixTime()` multiplied by 10^6 and 10^9, so every sub-second digit the
firmware puts on the wire is a zero. That is not merely imprecise, it is biased —
the reported time is always at or behind the true time, by up to a second — and it
lands in the two places the ground uses to judge the clock: the 1 Hz `SYSTEM_TIME`
and the `TIMESYNC` reply. `TIMESYNC` is worse than coarse: it answers with wall-clock
time, while both reference autopilots answer with monotonic time since boot, so the
offset a ground station computes from our reply does not mean what it thinks it
means.

Underneath that sit four defects that share one cause — there is no notion of *where
the current time came from*. `begin()`'s `_ds1307.begin() && RTC.begin()` short-circuits,
so with no DS1307 the internal RTC is never started at all and the degraded behaviour
`specs/fault-recovery/spec.md` already requires does not exist; `setUnixTime()` returns
early after comparing only the internal clock, so a drifted DS1307 — the one that seeds
the next boot — is never corrected from the ground; nothing validates an inbound
`SYSTEM_TIME`, so one corrupt frame puts every subsequent SD record in 1970; and
`systemTimeAvailable` in `src/main.cpp` is written once and read nowhere, which is the
absent provenance made visible.

## What Changes

- **Sub-second resolution from the RA4M1's own `R64CNT` counter.** `getUnixTimeUsec()`
  and `getUnixTimeNsec()` gain a fraction read from the RTC's `R64CNT` register
  (1/128 s = 7812.5 µs), phase-locked to the second it accompanies because the same
  divider chain produces both. Stateless: two register reads, no anchor to maintain,
  callable from any task. No new pin — the DS1307's `SQW` output is not reachable on
  this board and is not used.
- **A boot epoch, and monotonic time as a subtraction against it.** `lib/SystemTime`
  latches the wall time at boot and exposes time since boot derived from
  `wall − _bootEpoch`. `setUnixTime()` shifts the epoch by the same delta it applies
  to the clock, so an accepted time from the ground moves the wall clock without
  making time-since-boot jump. One `uint32_t` — not `time_t`, which is 8 bytes here and
  would not store in a single word — one writer, one aligned word: no accumulator, no
  wrap counter, no lock-free read protocol, no mutex.
- **A clock source ladder, replacing `systemTimeAvailable`.** Four values — ground,
  DS1307, survived (the internal RTC was already running with a plausible time when
  this boot started, which is what a watchdog or commanded reset leaves behind), and
  none. `begin()` selects one; an accepted ground time promotes to ground. This is
  modelled on ArduPilot's `AP_RTC::source_type`, including its ordering: a time from
  the ground outranks the hardware clock.
- **The internal RTC is started unconditionally**, and the DS1307 is written only when
  it is present. This is `add-degraded-mode` tasks 6.1/6.4, which were blocked on
  exactly this line, and it is what makes the `R64CNT` fraction readable in every
  configuration.
- **Inbound `SYSTEM_TIME` is validated** against a fixed sanity floor of
  `2022-01-01 00:00:00Z` before being accepted, after ArduPilot's
  `oldest_acceptable_date_us`. The same predicate does double duty: it is how `begin()`
  decides whether a still-running internal RTC survived a reset with something worth
  keeping, which is what lets a ground-set time outlive a reset with nothing persisted
  to `VBTBKR`.
- **`setUnixTime()` returns whether it accepted the time**, and no longer takes the early
  return that left a drifted DS1307 uncorrected for the whole mission. A time that differs,
  or that promotes the origin, reaches both clocks; a ground station repeating the second
  already held changes neither, because the reference GCS sends `SYSTEM_TIME` once a second
  and that repeat should not cost an I2C round trip. The return value is what the planned
  DataFlash log needs to emit its `TIME` record "on change", so it is part of the public
  surface from the start.
- **The clock's provenance is visible from the ground**: the boot `STATUSTEXT` names the
  source alongside the reset reason and boot phase it already carries, and a later
  change of source emits one. It also states, as a **separate** fact, whether a clock was
  already running with a plausible time when the firmware started — which is what makes
  the behaviour the survived source rests on observable on a board whose DS1307 is always
  attached, since the ladder otherwise hides it. A *rejected* time deliberately emits nothing — the sender
  controls how often it offers one, and a text per rejection would let a remote peer
  flood the write queue, which `mavlink-link` forbids. A rejection is observable anyway:
  the reported time does not move.
- **`TIMESYNC` is answered in monotonic time since boot, in nanoseconds, captured when
  the request is received** rather than when the reply is packed. `LinkMsg`'s
  `timesync` variant carries the captured `tc1` through the queue; `mavlinkPack()` in
  `src/mavlink.cpp` copies it instead of reading the clock, so `src/link.cpp` loses its
  one indirect reach for the clock and the writer task's latency stops being counted
  as clock offset.
- **`SYSTEM_TIME.time_unix_usec` carries `0` while the source is none**, the MAVLink
  convention for an unknown clock, rather than a small number a ground station would
  display as 1970. `time_boot_ms` continues to carry the useful figure.
- **A low-rate, bidirectional reconciliation of the two clocks**, so they do not drift
  apart for a whole mission with nobody bringing them together. The direction follows the
  ladder: with a ground-set clock the internal one is the authority and is written out to
  the DS1307, so the next boot seeds from something current; otherwise the DS1307 is the
  better keeper and seeds the internal one. This is the only place that job happens —
  doing it per inbound message is what made the reference GCS's 1 Hz updates expensive.
  It emits nothing on the link beyond an origin change, so it carries no spec requirement
  of its own: neither a ground station, an operator nor the build can observe whether a
  given reconciliation happened, which makes the interval a design decision rather than
  behaviour.
- **MAVLink surface.** No new message id, no changed rate, no changed identity triple.
  Three field-level changes: `TIMESYNC.tc1`'s time base and capture instant,
  `SYSTEM_TIME.time_unix_usec`'s unknown-clock value, and the clock source plus the
  found-running fact added to the boot `STATUSTEXT`.

**Non-goals.** `Data.uptime` and its 49.7-day wrap are untouched: the backlog entry
that replaces the `.mpk` log with a DataFlash format deletes that field outright, so
fixing it here would be work thrown away. Nothing is added to `VBTBKR` and
`include/Recovery.h`'s layout and checksum range are untouched. Selecting the RA4M1
sub-clock over `LOCO` for the internal RTC is investigated but not done here — a
stopped clock is a worse failure than a drifting one, so that is its own change if the
measurement supports it.

## Capabilities

### New Capabilities

- `system-clock`: what time the firmware reports, at what resolution, where that time
  came from, which source outranks which, what it accepts from the ground, and the
  continuity of time measured since boot. Nothing owns this today; the clock's
  behaviour is currently described only as a side effect of `mavlink-link`'s scenarios
  and `fault-recovery`'s degraded-hardware requirement.

### Modified Capabilities

- `mavlink-link`: the two scenarios stating that inbound `SYSTEM_TIME` and `TIMESYNC`
  "are acted upon" become conditional on validation, and the `TIMESYNC` answer gains a
  stated time base. These are on-wire changes, which is what this capability governs.

`fault-recovery` is deliberately **not** modified. Its requirement that absent hardware
degrades rather than halts, and its scenario for a clock that does not respond, are
already correct — this change makes the code match them for the first time, which is
implementation, not a requirement change. What time a degraded board reports, and how
the ground can tell, belongs to `system-clock`.

## Impact

**Files.** `lib/SystemTime/SystemTime.h` and `.cpp` (the whole of the above),
`src/mavlink.cpp` (inbound validation, `tc1` captured at receive, the unknown-clock
value, the re-seed schedule entry), `include/LinkMsg.h` (`tc1` in the `timesync`
variant), `src/main.cpp` (`systemTimeAvailable` replaced by the source),
`test/test_libs/test_main.cpp` and `test/test_hil/check_clock.py` /
`check_timesync.py` (new assertions; `check_timesync.py` today asserts only
`tc1 > 0`, which is why the wrong time base went unnoticed), `ARCHITECTURE.md`
§5.3 and §7, `TODO.md`.

**`ARCHITECTURE.md` §5.3 is already wrong** and this change is where it gets fixed: it
states that `begin()` "fails — hard, via `configASSERT` in `setup()` — if either does
not answer", which `add-degraded-mode` retired. §7's "the board runs on ticks since
boot" also stops being the mechanism, since a board with no RTC will run on an internal
RTC started at zero.

**RAM.** No task and no library is added. `LinkMsg`'s `timesync` variant grows by 8 bytes,
from 16 to 24, against a union whose size is set by the 52-byte `statustext` variant, so
`sizeof(LinkMsg)` does not change at all — measured with the target toolchain and confirmed
by `include/LinkMsg.h`'s `static_assert(sizeof(LinkMsg) <= 64)`.

What does move the figure is the **write queues going from depth 5 to 6**, one item per
port for the boot and origin-change texts, whose derivation lives beside the storage in
`src/main.cpp`: 2 x 64 = **128 B**. The clock's own state — a `uint32_t` epoch, the origin
enum and two flags — accounts for the remaining 4 B after padding. Measured against a
fresh link: headroom **2784 B**, down from a 2916 B baseline, against a floor of 1024.

**`TODO.md`.** *Improve clock synchronisation* is deleted by this change — all five of
its bullets are in scope, including the resynchronisation question, which is answered
rather than deferred. *Finish what add-degraded-mode left open* is **not** deleted: only
its 6.1 / 6.4 bullet is removed, and 6.4's board test becomes a task here. Two entries
need re-pointing rather than deleting: *The flight log is a private MessagePack format
no tool can read* defines its own three `Src` values and must take them from this
change's source ladder instead — it is missing the survived case entirely — and its
plan to extend the 32-bit tick counter "by an accumulator sampled at 1 Hz" is made
unnecessary by the boot-epoch subtraction. *`uptime` overflows after ~49.7 days* and
*Decide whether `SYSTEM_TIME` deserves 1 Hz* stay as they are.

**Other active changes.** None. `openspec/changes/` holds only the archive and this
change, so there is no other in-flight claim on a build flag, a queue depth or the RAM
budget for this one to invalidate.

**One requirement ships unexercised, and it is not an oversight.** The DS1307 on this
board cannot be disconnected — a standing property of the assembly, not a temporary
inconvenience — so no configuration exists in which the firmware finds no clock:
`src/main.cpp` calls `systemTime.begin()` unconditionally, the reduced configuration
included. `system-clock`'s requirement *An unknown clock is reported as unknown* therefore
has no scenario any of this project's three observers can run, and it is kept anyway
because a clock that cannot be unplugged on the bench can still die in orbit, which is
exactly the case it covers. Read it as intent, not as demonstrated behaviour. The
*outlives a reset* requirement was rewritten to be observable with the DS1307 attached,
via the found-running fact above, rather than joining it.

**Verification needs the board.** This touches `lib/SystemTime`, so `pio test -e libs`
applies — the destructive suite, which wipes `data*.mpk` and `index.bin` and must be
followed by `pio run -t upload`. Two facts this proposal rests on are assumptions until
measured on hardware: that the internal RTC keeps running across a software reset (which
the survived source depends on, and which `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` can
demonstrate in minutes), and how far the `LOCO`-driven internal RTC actually drifts
against the DS1307 (which decides whether the re-seed interval chosen here is right).
