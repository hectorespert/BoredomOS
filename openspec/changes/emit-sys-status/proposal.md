## Why

`SYS_STATUS` (1) is the most conspicuous message this firmware never sends: every
GCS shows it front and centre, and today the satellite sends none of it. Adding it
also surfaces two gaps that already exist quietly — a full write queue drops a
frame with nothing counting it, and there is no ground-visible signal for "the SD
card was never found" beyond the one-shot boot `STATUSTEXT`.

## What Changes

- Emit `SYS_STATUS` at 1000 ms on each port, unconditionally — including the
  reduced configuration — as a fifth entry in `TaskMavlink`'s per-port schedule
  table, alongside `HEARTBEAT` and `SYSTEM_TIME`.
- `onboard_control_sensors_present/enabled/health` report two bits, each mirrored
  across all three bitmaps (no separate present/enabled/health distinction exists
  in this firmware): `MAV_SYS_STATUS_LOGGING` for the SD card and
  `MAV_SYS_STATUS_SENSOR_BATTERY` for the battery ADC.
- No new state for the RTC: `MAV_SYS_STATUS_SENSOR` has no bit for a real-time
  clock, so this change does not touch how its absence is reported — that stays
  `STATUSTEXT`-only, exactly as today.
- `voltage_battery` and `battery_remaining` are filled from the same `Battery::`
  reads `sendBatteryStatus()` already takes, rather than left at the protocol's
  "not sent" sentinel.
- `errors_comm` is filled from `mavlink_get_channel_status(port)->packet_rx_drop_count`,
  a counter the vendored MAVLink C library already maintains per channel while
  parsing.
- `errors_count1` counts write-queue drops, one saturating `uint16_t` per port,
  via a new `enqueueWrite()` helper that replaces the 8 existing call sites that
  call `xQueueSend(linkPorts[port].writeQueue, &intent, 0)` without checking the
  return value. `errors_count2..4` stay `0` — no further candidate exists.
- `load` and `drop_rate_comm` stay unmeasured — this firmware's FreeRTOS build has
  no run-time-stats facility, and computing a drop-rate percentage from a counter
  pair that resets every 65536 packets is noise ArduPilot itself does not bother
  with.
- The rate is fixed in code at 1000 ms per port and is not made configurable via
  `MAV_CMD_SET_MESSAGE_INTERVAL` — out of scope for this change, and consistent
  with `HEARTBEAT`, `SYSTEM_TIME` and `BATTERY_STATUS`, none of which are
  configurable today either.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `mavlink-link`: adds `SYS_STATUS` as a periodic outbound message on both ports,
  at a fixed 1000 ms rate, unconditional on configuration — a new requirement
  alongside the existing per-port cadence guarantees.
- `fault-recovery`: the SD card's absence, already required to be reported
  ("Absent hardware degrades rather than halts"), becomes visible to a ground
  station that connects after the boot `STATUSTEXT` has already gone by, the same
  "state without asking for it" guarantee the heartbeat's degraded-mode fields
  already give — carried by `SYS_STATUS`'s sensor-health bitmap rather than by a
  message a late-connecting station would have missed.

## Impact

- **Code**: `src/mavlink.cpp` only — new `sendSysStatus()`, its schedule table
  entry, and the `enqueueWrite()` helper replacing 8 call sites. No other file
  changes.
- **MAVLink surface**: adds message id 1 (`SYS_STATUS`) to the periodic output of
  both ports. No change to the identity triple, to any existing message's rate,
  or to `COMMAND_LONG` handling.
- **RAM**: no new task or queue, but the existing per-port write queues grow
  from depth 6 to depth 7 — a fifth unconditional periodic entry
  (`sendSysStatus`) invalidates the depth-6 derivation in `src/main.cpp`,
  which assumed at most four periodic entries plus a `TIMESYNC` reply plus the
  boot clock report could coincide on one `TaskMavlink` pass (found during
  implementation, not anticipated at design time — see design.md). That costs
  `2 * sizeof(LinkMsg)` = 128 B; together with `writeDropCount[2]`'s 4 bytes,
  headroom moved from the 3180 B a `pio run` on this change's base commit
  reports to 3048 B, measured after both changes landed.
