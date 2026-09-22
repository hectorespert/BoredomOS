## 1. SYS_STATUS emission

- [x] 1.1 Write `sendSysStatus(uint8_t port)` in `src/mavlink.cpp`: builds the
      sensor bitmaps (`MAV_SYS_STATUS_LOGGING` from `sdCardAvailable`,
      `MAV_SYS_STATUS_SENSOR_BATTERY` always set), fills
      `voltage_battery`/`battery_remaining` from the same `Battery::` calls
      `sendBatteryStatus()` uses, and reads
      `mavlink_get_channel_status(port)->packet_rx_drop_count` for
      `errors_comm`. Verify with `pio run` (both environments) and `pio check`
      (no new findings against the current baseline). **Result:** both
      environments build clean, headroom unchanged at 3180 B (not yet wired
      into the schedule table), `pio check` still 2 LOW findings (unchanged
      baseline). `errors_count1` is a placeholder `0` here, wired to the real
      counter in task 2.1. Also found and fixed a spec gap while writing the
      pack call: `current_battery` (a field neither the proposal nor the spec
      named) has no data source in this firmware and is set to the protocol's
      `-1` sentinel, same reasoning as `load` — added to
      specs/mavlink-link/spec.md.
- [x] 1.2 Add `sendSysStatus` as a fifth entry to both ports' `schedule[...]`
      tables in `TaskMavlink`, at 1000 ms, `enabled: true` unconditionally
      (not gated by `reducedConfiguration`, unlike `sendBatteryStatus`).
      Verify with a HIL check: connect on each port and confirm `SYS_STATUS`
      arrives at roughly 1 Hz in both the normal and (hands-on, see 3.2) the
      reduced configuration. **Result:** flashed the board, ran the full HIL
      suite (`pio test`-equivalent `run.py`) on USB: `check_recovery.py`'s
      `test_observed_message_set_is_closed` failed first, because that case
      keeps a closed allow-list of message types and did not know
      `SYS_STATUS` — fixed by adding it there, noting it joins the
      unconditional set like `HEARTBEAT`/`SYSTEM_TIME`. After that fix, all
      38 pre-existing cases pass (9 ignored, unchanged — no adapter, no
      manual RESET, board not in reduced config). Reduced-configuration side
      not yet exercised — the board is not currently in that state; left for
      3.2's hands-on pass, which already needs the card pulled.
- [x] 1.3 Add a HIL case asserting `MAV_SYS_STATUS_LOGGING` is set in
      `onboard_control_sensors_present`/`_enabled`/`_health` when the board
      boots with the SD card present (the normal bench configuration), under
      `test/test_hil/`. **Result:** `test/test_hil/check_sys_status.py`,
      `test_sd_card_present_sets_the_logging_bit` — PASS.
- [x] 1.4 Add a HIL case asserting `voltage_battery`/`battery_remaining` in a
      `SYS_STATUS` sample match the values in a `BATTERY_STATUS` sample taken
      at the same time (within the two messages' independent schedules).
      **Result:** same file, `test_battery_fields_match_battery_status` —
      PASS, within a 50 mV / 2-point tolerance. Also added
      `test_battery_sensor_bit_is_always_set` and `test_sys_status_at_1hz`
      alongside it while in the file, covering task 1.2's cadence claim as an
      automated case rather than only the manual check above.

## 2. Write-queue drop counter

- [x] 2.1 Add `static uint16_t writeDropCount[kPortCount]` and
      `enqueueWrite(uint8_t port, const LinkMsg &intent)` in
      `src/mavlink.cpp`, saturating at `UINT16_MAX`; replace all 8 existing
      `xQueueSend(linkPorts[port].writeQueue, &intent, 0)` call sites with it.
      Wire `errors_count1` in `sendSysStatus` to `writeDropCount[port]`.
      Verify with `pio run` and `pio check` (no new findings). **Result:**
      both environments build clean; `pio check` unchanged at 2 LOW; headroom
      dropped from 3180 B to 3176 B, exactly the 4 bytes of
      `writeDropCount[2]` the design predicted. Flashed and re-ran the full
      HIL suite: 42/42, 9 ignored, unchanged.
- [x] 2.2 **Discovered while starting task 3.3, not planned at design time.**
      `sendSysStatus` is a fifth unconditional periodic `TaskMavlink` schedule
      entry; `src/main.cpp`'s write-queue depth (6) was derived from a
      worst-case count of exactly four such entries plus a `TIMESYNC` reply
      plus the boot clock report, with zero slack. Left alone, that would
      make emit-sys-status able to cause the very silent write-queue drop
      `errors_count1` exists to count. Fixed: depth 6 → 7 on both
      `uartWriteQueue`/`usbWriteQueue` (storage arrays and
      `xQueueCreateStatic` calls in `src/main.cpp`), with the derivation
      comment re-written to include the new entry, per CLAUDE.md's
      "re-derived, not assumed" rule. Also moved `sendSysStatus`'s schedule
      seed off `sendHeartbeat`'s own seed (both 1000 ms) to a 750 ms offset,
      so the two are not due on the same pass every single second in steady
      state — same reasoning as `sendSystemTime`'s existing 500 ms offset.
      **Verified:** both environments build clean; `pio check` unchanged at 2
      LOW; headroom moved from 3176 B to 3048 B (128 B = `2 * sizeof(LinkMsg)`,
      matching the prediction). Flashed and re-ran the full HIL suite: first
      run caught a second, unrelated bug — `test_battery_fields_match_
      battery_status`'s tolerance (50 mV / 2 points) was tighter than the
      board's real ADC jitter between two reads a fraction of a second apart;
      fixed by deriving the percent tolerance from the voltage tolerance
      through `remaining()`'s own known slope (100/700 %/mV) instead of a
      guessed number — 5/5 clean re-runs after. Full suite: 43/43, 9 ignored,
      unchanged.
- [ ] 2.3 Add a HIL case that deliberately floods a port faster than its
      write queue drains (e.g. rapid repeated `COMMAND_LONG` requests that
      each queue a `COMMAND_ACK`) until at least one drop is forced, and
      asserts the next `SYS_STATUS` on that port reports a higher
      `errors_count1` than one taken before the flood. If this cannot be
      forced reliably from a HIL case within the existing queue depth and
      schedule, record that here rather than dropping the check silently, and
      fall back to an ad hoc verification script run by hand against the
      board (same approach `answer-unsupported-command-long-requests` used
      for its stack high-water-mark check) to confirm the counter increments
      at least once before calling this task done.
      **Result: could not be forced, by any approach tried.** Three ad hoc
      scripts against the board (not committed — scratchpad only), all run
      before 2.2's queue-depth fix, against the then-current depth-6 queue:
      1. A 60-command instant burst (0.18 s) — `errors_count1` stayed 0.
      2. A 3000-command sustained flood at the Python client's own ceiling
         (~243 msg/s over 12.3 s) — still 0.
      3. Not draining the host side at all for 20 s, relying on periodic
         traffic alone (~3.5 msg/s combined) to back the queue up — still 0,
         consistent with the existing `mavlink-link` spec guarantee that a
         non-draining host must not stall any task; outbound frames are
         discarded at a layer below this queue when the host is not reading,
         not by backing the write queue up.
      A fourth, diagnostic script confirmed why (2) does not work: flooding
      500 commands at ~251/s produced 500/500 `COMMAND_ACK`s with zero loss
      — the firmware's read-process-ACK-write pipeline keeps up with
      whatever a Python/pymavlink client can generate per call, so the
      client's own per-message overhead (~250-300 msg/s ceiling, not the
      firmware) is the limiting factor, and it never gets ahead of a queue
      draining every loop pass — now depth 7, an even harder target than the
      depth 6 these attempts already failed against. Forcing a genuine
      overflow would need either raw-byte throughput well beyond what
      `command_long_send()` in a Python loop can sustain, or reducing the
      queue depth for the test — the former is disproportionate effort for
      one counter, the latter would test a different firmware than what
      flies. Left undone rather than backed by a check that does not
      actually exercise the failure path; `errors_count1`'s logic is still
      covered indirectly by 2.1's `pio check`/build verification and code
      review of `enqueueWrite()`.

## 3. Fault-recovery guarantee and documentation

- [x] 3.1 Add a HIL case asserting a `SYS_STATUS` received on a fresh
      connection already reports `MAV_SYS_STATUS_LOGGING` correctly — i.e. no
      request is needed to see current SD-card presence, matching the
      "state without asking for it" scenario added to `fault-recovery`.
      **Result:** `check_sys_status.py`, `test_sd_presence_is_visible_without_
      asking` — PASS. Docstring notes the same harness-level caveat
      `check_recovery.py` already documents: `run.py` consumes a few
      seconds of traffic before any case runs, so this shows no request is
      sent, not that none was needed since power-on.
- [ ] 3.2 **Needs hands, not just the board**: with the SD card physically
      removed before boot, confirm `MAV_SYS_STATUS_LOGGING` is clear in
      `SYS_STATUS` on both ports, then reinsert the card and reboot to leave
      the bench in its normal configuration afterward.
- [x] 3.3 Update `ARCHITECTURE.md`'s description of `TaskMavlink`'s per-port
      schedule to mention `sendSysStatus`, and update `TODO.md` — delete the
      "Emit `SYS_STATUS`" entry in the same commit that archives this change,
      per this project's rule that a picked-up entry is not described twice.
      **Result:** while starting this task, found the `TODO.md` deletion was
      already overdue — CLAUDE.md's rule is to delete the entry when the
      change is *proposed*, not when it is archived (this tasks.md item had
      it at the wrong stage); corrected by deleting it now, and re-pointing
      the one cross-reference to it (`SD logging failure is silent`'s "see
      Emit SYS_STATUS above") at `openspec/changes/emit-sys-status` — worded
      to say plainly that this change's bitmap is boot-time presence only, so
      it does not close that entry's need for a *live* failure signal.
      `ARCHITECTURE.md` updated in three places: the task table row, the
      `TaskMavlink` schedule prose (now naming `SYS_STATUS` and its 750 ms
      offset, "fourth"/"fifth entry" renumbered), and §4's queue depth/size
      figures (6→7, 384 B→448 B) — the same figures task 2.2 changed in code.
