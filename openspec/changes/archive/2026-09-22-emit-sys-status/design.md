## Context

See proposal.md for motivation. Current state this design builds on:

- `TaskMavlink`'s per-port `schedule[kPortCount][4]` table ([src/mavlink.cpp](../../../src/mavlink.cpp))
  already carries `sendHeartbeat`/`sendSystemTime` unconditional and
  `sendBatteryStatus` gated off `reducedConfiguration`.
- 8 call sites call `xQueueSend(linkPorts[port].writeQueue, &intent, 0)` and
  discard the return value.
- `include/Recovery.h` is the one existing precedent for state shared across
  tasks: written once in `main.cpp` before the scheduler starts, read-only after
  — no mutex, because nothing writes it again once `TaskMavlink` is reading it.
- `sdCardAvailable` (`src/main.cpp`) already follows that same write-once
  lifetime; nothing equivalent exists for the RTC today.
- The vendored MAVLink C library's `mavlink_status_t` (per channel, via
  `mavlink_get_channel_status(chan)`) already maintains `packet_rx_drop_count`
  during parsing, unrelated to anything this firmware does.
- This firmware's FreeRTOS build has no `configGENERATE_RUN_TIME_STATS` and no
  project-owned `FreeRTOSConfig.h` to add it to — CPU load is not measurable
  without a core change out of scope here.

## Goals / Non-Goals

**Goals:**
- Implement the `mavlink-link` and `fault-recovery` deltas as specified.
- Reuse the two write-once-before-scheduler precedents (`Recovery`,
  `sdCardAvailable`) rather than introducing the firmware's first cross-task
  shared mutable state, and reuse `sdCardAvailable` itself rather than
  duplicating it or adding anything equivalent for the RTC.
- Reuse counters the MAVLink library already maintains instead of duplicating
  them.

**Non-Goals:**
- CPU load (`load`) — no measurement mechanism available.
- A configurable `SYS_STATUS` rate via `MAV_CMD_SET_MESSAGE_INTERVAL`.
- A real health signal for the RTC or a live (post-boot) health signal for the
  SD card — both stay boot-time presence only, matching `sdCardAvailable`'s
  existing scope.
- `drop_rate_comm` as a computed percentage.

## Decisions

**Reuse `sdCardAvailable` directly; add no new global for the RTC.**
`sdCardAvailable` already is exactly the boot-time-only, write-once-before-
scheduler bool this needs for the one bit this firmware's `SYS_STATUS` reports
about physical presence — a second variable duplicating it would just be two
names for one fact, free to drift apart. `SYS_STATUS`'s sensor bitmap has no bit
for a real-time clock, and this change does not touch how the RTC's absence is
reported (still `STATUSTEXT`, still `systemTime.source()`), so nothing new needs
reading from `SystemTime` — the fact already exists as the private
`_ds1307Present` and stays private. Earlier drafting of this design proposed
exposing it as a public accessor and a new `rtcPresent` global; both were
removed once it became clear during implementation that nothing in this
change's actual scope would read them — see tasks.md's history for that
correction.

**Stays boot-time-only, not runtime-updated.** With `present == enabled ==
health` for every bit this firmware reports (decided in exploration: nothing in
this firmware is toggled at runtime, and no post-boot health check exists for
either resource), there is nothing for a second writer to update after boot.
This keeps the change inside the existing `Recovery`-style lifetime instead of
introducing the first cross-task shared mutable state the firmware would have —
the alternative (a `volatile` flag written by `TaskSdWrite` on every failed
`sdData.write()`) was considered and rejected as unnecessary scope: it would
require this change to also start checking `write()`'s return value, which
nothing does today, for a live-health signal the design explicitly does not
need.

**`enqueueWrite(port, intent)` helper replacing the 8 unchecked `xQueueSend`
call sites.** All 8 do the same thing and need the same new behaviour (count a
drop); a shared helper keeps that logic in one place rather than repeating a
saturating-increment at each site.

**`errors_comm` read from `mavlink_get_channel_status(port)->packet_rx_drop_count`.**
The library already maintains this per channel while parsing inbound frames;
nothing in this firmware needs to duplicate it. ArduPilot's `send_sys_status()`
reads the same field for the same purpose, confirming it is the field's intended
use rather than an internal implementation detail unsafe to depend on.

**`voltage_battery`/`battery_remaining` filled from `Battery::`, not left at the
protocol's sentinel.** MAVProxy and QGroundControl — the two GCS this project
targets — both read battery state exclusively from `BATTERY_STATUS` and ignore
these two `SYS_STATUS` fields entirely (verified against both projects' current
source). Left alone, that would argue for the sentinel. ArduPilot fills them
anyway (`GCS_Common.cpp`, `send_sys_status()`), which is the deciding factor:
matching the reference autopilot's behaviour costs one more `Battery::` read per
second and protects against a GCS or tool this project does not currently target
but might later, that does read `SYS_STATUS` for battery state.

**`drop_rate_comm` and `errors_count2..4` stay `0`.** `drop_rate_comm` could be
computed from `packet_rx_success_count`/`packet_rx_drop_count`, but both are
`uint16_t` counters that reset together every 65536 successful packets — a
ratio computed from them is noisiest exactly at each reset. ArduPilot, which has
the same two counters available, also leaves this field `0` rather than compute
it. `errors_count2..4` have no candidate source in this firmware.

**Bit choice verified against the reference GCS's own display code, not just
the enum's names.** `MAV_SYS_STATUS_LOGGING` for the SD card and
`MAV_SYS_STATUS_SENSOR_BATTERY` for the battery sense were chosen from
`MAV_SYS_STATUS_SENSOR`'s available bits; MAVProxy's `mavproxy_console.py`
independently maps `MAV_SYS_STATUS_LOGGING` to the "LOG" indicator it already
shows, confirming the choice lines up with what the reference GCS surfaces
rather than being an arbitrary pick among several equally-poor fits. No bit
exists for the RTC; its absence stays `STATUSTEXT`-only, as stated in the spec
delta.

**File ownership** (per this project's "only the owning file touches its
resource" rule): `src/main.cpp` remains the sole writer of `sdCardAvailable`,
before the scheduler starts. `lib/SystemTime` remains the sole owner of
`_ds1307Present`, unchanged and still private — this change adds no reader for
it. `src/mavlink.cpp` remains the sole reader of `sdCardAvailable`, the sole
caller of `mavlink_get_channel_status()`, and the sole place `writeDropCount` is
written or read — no new file touches a peripheral or a second file's state.

**The per-port write queues grow from depth 6 to depth 7.** Found while
implementing, not anticipated while writing this design: `src/main.cpp`'s
comment beside `uartWriteQueueStorage` derives depth 6 from an explicit
worst-case count — four independently-clocked `TaskMavlink` schedule entries
that "nothing in the schedule stops... coinciding on one pass," plus a
`TIMESYNC` reply, plus the boot clock report — with zero slack. `sendSysStatus`
is a fifth such entry, unconditional like `HEARTBEAT`/`SYSTEM_TIME` rather than
gated like `BATTERY_STATUS`, so it cannot be excluded from that worst case.
Shipping it without widening the queue would make a `SYS_STATUS` (or another
message sharing its unlucky pass) silently droppable in exactly the scenario
`errors_count1` exists to count — the new feature would occasionally cause the
failure it also measures. Re-derived to depth 7, costing
`2 * sizeof(LinkMsg)` = 128 B more `.bss`, following the same "re-derived, not
assumed" rule `src/main.cpp`'s comment already states for this queue.

`sendSysStatus`'s schedule seed was also moved off `sendHeartbeat`'s own seed
(both being 1000 ms) to a 750 ms offset from it, once this was noticed: sharing
a seed would make the two due on the very same pass every single second in
steady state, not just as a worst case — the same reason `sendSystemTime`
already carries its own 500 ms offset rather than sharing `sendHeartbeat`'s.
This does not change the required queue depth (worst-case coincidence is a
property of the schedule's interval numbers, not of nominal phase, since
`TaskMavlink`'s own loop can still align any offsets under jitter) — it only
avoids a routine, entirely avoidable doubling of traffic every second.

## Risks / Trade-offs

- **The dropped-frame scenario is hard to trigger from the ground under normal
  traffic** → a HIL case for it needs to deliberately flood a port faster than
  its write queue drains, similar in spirit to the ad hoc verification script
  `answer-unsupported-command-long-requests` used for its stack high-water-mark
  check. tasks.md accounts for this as its own step rather than assuming a
  simple request/response case covers it.
- **`SYS_STATUS` and `BATTERY_STATUS` read `Battery::` independently, on
  different schedule entries (1000 ms vs 2000 ms)** → the two messages can show
  a reading a few hundred milliseconds apart rather than bit-identical at every
  instant. Battery voltage does not move fast enough for this to matter, and the
  spec's wording ("the same battery state `BATTERY_STATUS` reports") is written
  to tolerate this rather than demand a shared read.
- **A GCS that does read `load`/`drop_rate_comm` will see `0`, indistinguishable
  from a real zero reading** → not resolvable without the FreeRTOS core change
  this design deliberately excludes; ArduPilot carries the identical ambiguity
  for `drop_rate_comm` today.
