Steps marked **[board]** need the assembled board attached. Steps marked **[hands]**
additionally need someone at the board — pressing RESET, pulling the card, forcing the
reduced configuration. `pio run` and `pio check` are the only checks that run without
hardware.

**Where `pio run` runs.** `toolchain-gccarmnoneeabi@1.70201.0` is an x86_64 binary, so
on an Apple Silicon host it needs Rosetta — which macOS 27 uninstalls when it upgrades.
Without it nothing compiles, nothing flashes and no `[board]` step is reachable;
`softwareupdate --install-rosetta` restores it. See *The toolchain and uploader are
x86_64-only* in `TODO.md` for why that is a deadline and not just an inconvenience.

If a host is ever without it again, every `pio run` verification below can be read from
CI instead, which builds all three environments on `ubuntu-latest`:

```
gh run list --workflow=main.yml --limit 3
gh run view <id> --log | grep -A4 "RAM budget"
```

**This change lands before `add-usb-dual-protocol`.** Built the other way round, that
change fails `scripts/ram_budget.py`, which is why this one exists.

## 1. Baseline, before anything is edited

Without these the questions section 5 asks have no answer. Take them first.

- [x] 1.1 **[board]** Record `ps` for `Heartbeat`, `MavlinkBatteryStatus`, `Mavlink`
      and `Logger` — the unused-stack figures are what 5.1 and 5.11 compare against,
      and `TODO.md` records only two of them (`Heartbeat` at 28 free of 128,
      `Logger` at 5 of 96) — and record `free`'s minimum-ever-free heap; verify by
      pasting the four rows and the `free` reply into the commit message.
      **Sequencing note:** this reading was taken after 2.1-4.3 had already been
      implemented and verified by `pio run`, not before — the session did not have
      board access until this point. Recovered a genuine pre-change reading anyway:
      `git stash push` on just the six build-affecting source files (not `TODO.md`,
      `ARCHITECTURE.md` or this change's own `tasks.md`), flashed the resulting
      pre-change tree to the flight environment, read the CLI, then `git stash pop`
      and reflashed the post-change tree. Confirmed:
      ```
      ps Heartbeat            : 3  Heartbeat  PRI 2  B  28
      ps MavlinkBatteryStatus : 6  MavlinkBat PRI 2  B  45
      ps Mavlink              : 4  Mavlink    PRI 1  B  205
      ps Logger               : 7  Logger     PRI 1  B  5
      free                    : heap total 6144, heap free 6136, heap free min 5224
      ```
      `Heartbeat` and `Logger` match `TODO.md`'s recorded figures exactly (28/128,
      5/96); `MavlinkBatteryStatus` (45/128) and `Mavlink` (205/256) are new data
      points `TODO.md` did not have
- [ ] 1.2 **[board]** Capture a reference trace with `mavproxy.py` on the radio link:
      `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz with `SYSTEM_TIME` falling ~500 ms after
      each `HEARTBEAT`, and `BATTERY_STATUS` every 2 s; verify by recording the
      observed intervals, since 5.4 has to match them.
      **Left unticked, honestly**: this needed a pre-change capture, and by the time
      this session had board access the pre-change code was already reverted for
      1.1's baseline but never re-captured for a MAVLink trace — only the CLI
      readings, since capturing a trace also needs re-flashing to `bench` and this
      session prioritised not multiplying flashes further. 5.4 used a different,
      arguably stronger comparison basis instead (this project's own recorded
      pre-change CI passes of the same behavioural requirements, plus a fresh direct
      capture) — see 5.4's note. Do not check this box on the strength of that
      substitution; if a literal pre-change trace is wanted later, it is one more
      `git stash` + `pio run -e bench -t upload` + `cadence_probe.py` away
- [x] 1.3 Confirm the pre-change baseline is still `headroom 1468 B` / `committed
      31300 B of 32768`. The last green run on `master` reports exactly that, so this
      is a re-read rather than a measurement unless the tree has moved since; verify
      with `gh run view <id> --log | grep -A4 "RAM budget"` and correct `proposal.md`
      if it disagrees. Confirmed against run 34879410471 (the last green run on
      `master`, 2026-09-14): `committed 31300 B of 32768 (95.5%)`,
      `headroom 1468 B (minimum 1024)`, identical across all three environments.
      Matches `proposal.md` exactly, no correction needed

## 2. The schedule inside `TaskMavlink`

- [x] 2.1 Add the `{function, interval_ms, last_ms}` table to `src/mavlink.cpp` with
      `sendHeartbeat` and `sendSystemTime` at 1000 ms and `sendBatteryStatus` at
      2000 ms, seeding `sendSystemTime`'s `last_ms` half an interval behind so the
      500 ms interleave of today is preserved; verify with `pio run`
- [x] 2.2 Replace `TaskMavlink`'s `portMAX_DELAY` with the time to the next deadline,
      floored at zero, and emit every entry that is due after each receive returns or
      times out; verify with `pio run`
- [x] 2.3 Advance a fired entry with `last_ms += interval_ms` rather than to the
      current tick, so a late pass does not push the cadence forward — this is what
      replaces `vTaskDelayUntil`'s drift-free property; verify with `pio run` and by
      inspection that no branch assigns the current tick to `last_ms`
- [x] 2.4 Move the stability-window clear and the 30-minute reduced-configuration
      retry out of `TaskHeartbeat` into the same loop, keeping the reasoning at
      `src/mavlink.cpp:196` with them and restating the invariant they now depend on:
      the `HEARTBEAT` entry is present in every configuration and never conditional;
      verify with `pio run`
- [x] 2.5 Make the `BATTERY_STATUS` entry conditional on the reduced configuration in
      the table rather than at task creation; verify with `pio run` and by inspection
      that `setup()` no longer decides which telemetry exists

## 3. Reclaim the RAM

- [x] 3.1 Remove the `TaskHeartbeat` and `TaskMavlinkBatteryStatus` bodies from
      `src/mavlink.cpp`, leaving `sendHeartbeat()`, `sendSystemTime()` and
      `sendBatteryStatus()` untouched; verify with `pio run` and with the CI grep that
      no `xTaskCreate` or `xQueueCreate` appeared in `src/`
- [x] 3.2 Remove from `src/main.cpp` the two `StackType_t` arrays, the two
      `StaticTask_t`, the two `TaskHandle_t`, the two `[[noreturn]] extern`
      declarations and the two `xTaskCreateStatic` calls with their `configASSERT`s;
      verify with `pio run`
- [x] 3.3 Change `TaskMavlink`'s priority from `PRIORITY_LOW` to `PRIORITY_HIGH` in
      `src/main.cpp`; verify with `pio run` and by inspection that the value comes
      from `include/Priority.h` and not a number
- [x] 3.4 Add `-D MAVLINK_COMM_NUM_BUFFERS=1` to `build_flags` in `platformio.ini`.
      The default is 4, which reserves `mavlink_message_t m_mavlink_buffer[4]` — 1164
      bytes in the current image, read from its symbol — plus a
      `m_mavlink_status[4]` of 96 bytes per translation unit that instantiates it, of
      which the image holds two. This firmware parses one channel: `MAVLINK_COMM_0`
      at `src/serial.cpp:41` is the only `MAVLINK_COMM_*` reference in `src/`. Verify
      with `pio run` and by confirming `m_mavlink_buffer` fell to 291 bytes in the
      map file — not by trusting the arithmetic below. Confirmed: `firmware.map`
      shows `m_mavlink_buffer` at `0x123` = 291 B, and two `m_mavlink_status` copies
      (`mavlink.cpp.o`, `serial.cpp.o`) at `0x18` = 24 B each, 48 B total
- [x] 3.5 Record in `platformio.ini` beside the flag that `add-usb-dual-protocol`
      raises it to `2` when it adds `MAVLINK_COMM_1`, so the next reader does not
      discover the constraint by breaking it; verify the comment names that change
- [x] 3.6 Confirm the build's reported headroom rose by **at least 2065 bytes** — 1192
      from the two tasks plus 873 from the channel buffer — to roughly 3533, and by up
      to 2209 if both `m_mavlink_status` copies collapse. Verify against the figure
      recorded in 1.3; a smaller rise means something was not actually removed.
      Confirmed: headroom rose from 1468 B to 3684 B, a rise of 2216 B — above the
      2065 floor, and close to the 3677 upper estimate (1468 + 1192 + 873 + 144):
      both `m_mavlink_status` copies did shrink from 96 B to 24 B each (144 B freed,
      not merely "up to"), with the remaining ~7 B from the two `TaskHandle_t`
      globals `taskHeartbeatHandler`/`taskStatusHandler` also removed in 3.2

## 4. The housekeeping log

- [x] 4.1 Remove `heartbeatAvailableStack` and `statusAvailableStack` from `Tasks` in
      `include/Data.h`, taking it from seven fields to five; verify with `pio run`
- [x] 4.2 Remove the two `extern TaskHandle_t` declarations and the two
      `uxTaskGetStackHighWaterMark` samples from `src/logger.cpp` (lines 10, 12, 32,
      33); verify with `pio run`
- [x] 4.3 Remove the two `JsonDocument` fields from `src/sdwrite.cpp` (lines 27, 28);
      verify with `pio run`

## 5. Verification on the board

- [x] 5.1 **[board]** Read `Mavlink`'s unused stack from `ps` and compare against the
      worst of the three figures from 1.1; verify there is margin left in 256 words,
      and if there is not, grow the array and the word count together and re-measure
      rather than assuming. Confirmed: `ps Mavlink` reports **119 free of 256**
      words (used ~137) — comfortable margin, well clear of zero. 256 words holds;
      no resize needed
- [x] 5.2 **[board]** Run `free` after several minutes and confirm the minimum-ever-
      free heap is no worse than 1.1's — no allocation pattern changed, so a
      difference here means something did. Confirmed: `heap free min: 5224` both
      immediately after flashing and again after a 5-minute idle wait — identical to
      1.1's pre-change reading (also 5224). No leak, no new allocation pattern
- [x] 5.3 **[board]** Confirm `ps` lists six tasks plus `IDLE` where it listed eight,
      with no CLI source file edited to make that true. Confirmed: `ps` reports
      seven rows — `Cli`, `IDLE`, `SerialRead`, `Logger`, `Mavlink`, `SdWrite`,
      `SerialWrite` — six real tasks plus `IDLE`. `src/cli.cpp` was not touched by
      this change
- [x] 5.4 **[board]** With `mavproxy.py` on the radio link, confirm the rates and the
      500 ms interleave match 1.2 exactly; verify by comparing the observed intervals,
      not by confirming the messages merely arrive. **Substitution noted:** run over
      `pio test -e bench`'s USB link with a small `pymavlink` script instead of
      `mavproxy.py` by hand, and against `check_telemetry.py`'s assertions rather
      than a manually paired 1.2 trace — 1.2 was not separately captured (see 1.1's
      sequencing note), and that suite already encodes the same acceptance criteria
      (exact 1 Hz / 2 s) more precisely than reading a console by eye. Both ran
      clean: this session's own bench HIL run passed `test_heartbeat_at_1hz`,
      `test_system_time_at_1hz` and `test_battery_status_every_2s`, and a direct
      16 s capture (below, shared with 5.6) shows `HEARTBEAT` at exactly `x.62s` and
      `SYSTEM_TIME` at exactly `x.12s` for 15 consecutive intervals each — 1.000s
      every time, 500 ms apart, no drift
- [x] 5.5 **[board]** Confirm the identity triple is unchanged — system `1`,
      `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`, `MAV_AUTOPILOT_GENERIC` — and that
      MAVProxy still sees one vehicle with no new component. Confirmed twice: this
      session's bench HIL run passed `test_one_vehicle_with_the_expected_identity`
      and `test_vehicle_type_is_rocket`, and the direct capture below saw exactly one
      identity, `(system=1, component=1)`, and `HEARTBEAT.type == 9` (`MAV_TYPE_
      ROCKET`) on every one of 16 heartbeats
- [x] 5.6 **[board]** Drive an inbound `SYSTEM_TIME` that actually changes the clock
      and a `TIMESYNC` round trip, and confirm the heartbeat cadence does not slip:
      this is the I2C risk `design.md` names, and it is the one behaviour the fold
      could plausibly break. Confirmed with a purpose-built capture
      (`cadence_probe.py`): 8 s of baseline, then a `SYSTEM_TIME` set to `now + 1h`
      (clear of `setUnixTime()`'s unchanged-value short-circuit, so this forces a
      real DS1307 write over I2C) plus a `TIMESYNC` round trip at `t=8.12s`, then 8 s
      more. Every `HEARTBEAT` and `SYSTEM_TIME` interval stayed exactly **1.000s**
      through the stimulus — no stall, no slip, including the pass immediately
      after the write. `TIMESYNC` was answered once, as expected
- [ ] 5.7 **[board] [hands]** Force the reduced configuration (three consecutive
      unstable boots, or ten cumulative resets) and confirm `BATTERY_STATUS` stops
      while `HEARTBEAT` and `SYSTEM_TIME` continue, and that `ps` shows the SD tasks
      absent and `Mavlink` present
- [ ] 5.8 **[board] [hands]** Pull the card, boot, and confirm the firmware still
      reaches steady state with the schedule intact
- [ ] 5.9 **[board] [hands]** Read a fresh `data*.mpk` back and confirm the records
      carry five per-task fields, and that a card still holding seven-field records
      from before the change is written to without error
- [x] 5.10 **[board]** Read `Logger`'s unused stack and compare against 1.1.
      `include/Data.h` shrinks by two `UBaseType_t` in section 4, and `TODO.md`'s
      *`TaskLogger`'s stack margin is razor-thin* — 5 free of 96 words — asks
      explicitly to re-check after any change to `Data`'s size. The margin should
      improve slightly; verify that it did rather than assuming, and record the figure
      in that entry. Confirmed: `ps Logger` reports **6 free of 96** words, up from
      5 in 1.1 — the predicted small improvement, not assumed. Recorded in `TODO.md`'s
      *`TaskLogger`'s stack margin is razor-thin* entry
- [ ] 5.11 **[board] [hands]** The 30-minute retry moved in 2.4 and **is not verified
      by this change.** Observing it needs an uninterrupted capture spanning two retry
      intervals, which is the debt `TODO.md` already records against
      `add-degraded-mode` task 8.4. Confirm by inspection that the code path moved
      intact and leave this unticked rather than claiming a check that was not run.
      The by-inspection half is done — 2.4's diff moved `Recovery::setConsecutiveCount`
      and the `RETRY_INTERVAL_MS` branch into `TaskMavlink`'s loop with the
      reasoning comment intact, and 5.7 (still open) is what would show the counter
      side; the two-retry-interval live observation this box actually asks for was
      not run and is left unticked per its own instruction, not attempted and
      forgotten

## 6. Suites and analysis

- [x] 6.1 **[board]** Run `pio test` and confirm all nine HIL cases pass. The flight
      build needs a USB-TTL adapter on D0/D1; `pio test -e bench` puts the link on USB
      instead, and `pio run -t upload` afterwards restores the flight configuration.
      This suite passing unchanged is the evidence that the change is invisible from
      the ground. Ran with `pio test -e bench` — no USB-TTL adapter was attached
      (`system_profiler SPUSBDataType` showed only the board's own CDC port).
      Result: **14 cases, 10 passed, 0 failed, 4 skipped**. The four skips
      (`check_cli.py`'s two cases, `check_silence.py`'s one, and
      `check_recovery.py`'s reduced-configuration case) are the bench build's own
      designed self-skips — CLI moves to `Serial1` and the link carries MAVLink
      instead of silence when `LINK_SERIAL=Serial`, and the board was not in the
      reduced configuration — not something this change caused. The ten that ran
      cover exactly the surface this change touches: `HEARTBEAT`/`SYSTEM_TIME` at
      1 Hz, `BATTERY_STATUS` every 2 s, the identity triple, `TIMESYNC`, inbound
      `SYSTEM_TIME` setting the clock, and the closed message set — all passed.
      **Not yet run: `pio test` on the flight build**, which needs the USB-TTL
      adapter this session does not have; that is a narrower remaining check, not a
      substitute for this one. `pio run -t upload` still owed before the board is
      left in flight configuration
- [x] 6.2 Run `pio check` and confirm no new finding against the pre-change baseline;
      verify by comparing counts, noting that `TODO.md` already records the baseline
      as 13 LOW findings rather than 12. First pass found 14 (one new: `entry` in the
      due-date scan could be a const reference); fixed by declaring it
      `const ScheduleEntry &entry`. Re-run: 13 LOW (1 `lib/Battery`, 12 `src`), same
      as baseline, no new finding
- [x] 6.3 Confirm `pio run` builds all three environments, as CI does. All three
      `SUCCESS`: `uno_r4_minima` headroom 3684 B, `bench` headroom 3684 B (Flash
      91660 B — lower than flight's 92332 B, from `LINK_SERIAL=Serial`/
      `CLI_SERIAL=Serial1` substituting different code paths), `libs` headroom
      3684 B (identical to flight — `pio run` is unaffected by `test_build_src`,
      which only changes what `pio test` links)
- [x] 6.4 `pio test -e libs` is **not** run: it covers `lib/` only, this change touches
      no library, and it would delete the card's contents that 5.9 depends on.
      Confirmed not run this session

## 7. Documentation

- [x] 7.1 Update `ARCHITECTURE.md`: the task table drops to six rows and `Mavlink`
      becomes `HIGH` with a cadence; §3's priority rationale; §3's "Consumer tasks
      block on `xQueueReceive` with `portMAX_DELAY`" sentence, now false for this
      task; §4's producer count for `serialWriteQueue` and the heap figure that falls
      from 5808 to 5200; §5.1's description of the three tasks; §5.2's seven per-task
      fields. Verify by re-reading it against the built firmware. Also updated: the
      §2 mermaid diagram (dropped the `HB`/`BS` nodes and their edges), §3's
      `TaskSerialWrite` rationale ("its own producers" → "its only producer"), and
      the reduced-configuration paragraph naming `TaskHeartbeat` by a task that no
      longer exists
- [x] 7.2 Re-point the `TODO.md` entries that name the tasks this change deletes.
      *Decide whether `SYSTEM_TIME` deserves 1 Hz* already exists and already
      references this change; *Improve clock synchronisation* says `SYSTEM_TIME` is
      "emitted every second from `TaskHeartbeat`", which will name a task that no
      longer exists. Verify by grepping `TODO.md` for `TaskHeartbeat` and
      `TaskMavlinkBatteryStatus` and confirming no reference dangles.
      Six entries updated: *Answer the GCS messages that are ignored today*,
      *Improve clock synchronisation*, *The SD log never stores the battery data*,
      *`TaskLogger`'s stack margin is razor-thin*, and both `TaskMavlinkBatteryStatus`
      references in *The flight log is a private MessagePack format no tool can
      read* — the latter also corrected the producer count for `sdWriteQueue` from
      three to two, since `PWR` and `TIME` now come from the same task. The two
      remaining `TaskHeartbeat` mentions (`TaskLogger`'s stack-margin entry, now
      explicitly past tense) and the two in the `Done` section are historical record
      and left as-is per `TODO.md`'s own rule that nothing new is added there
- [x] 7.3 Reword what this change invalidates in `add-usb-dual-protocol`:
      `design.md:106-107`, task 2.2's "both producer tasks", task 2.3's "reduce
      `TaskMavlink` to receive / dispatch / queue-the-reply / free", and add to it the
      step of raising `MAVLINK_COMM_NUM_BUFFERS` to `2` alongside its second parser
      state, with the 315 bytes that costs entered in its RAM table. That change and
      this one now both claim the same build flag, which is the collision
      `openspec/config.yaml` warns has already happened once undetected. Verify the
      two changes no longer describe different worlds. Done: `design.md`'s schedule-
      ownership section and its "second parser channel" section (now naming the
      315 B figure and the required flag raise, precisely), `proposal.md`'s Impact
      table (315 B row, new 1083 B total, and a note that the change still fits
      against the larger headroom this change leaves), and `tasks.md` — reworded 2.2
      and 2.3, and inserted a new task 3.1 to raise the flag, renumbering the rest of
      section 3 (old 3.1-3.4 → 3.2-3.5). Checked no stale task-number cross-reference
      was left by the renumbering
- [x] 7.4 Run `openspec validate fold-periodic-telemetry-into-mavlink-task --strict`
      and confirm it is clean with `skip_specs: true` accounted for. Clean: "Change
      'fold-periodic-telemetry-into-mavlink-task' is valid", one INFO noting
      `skip_specs` is honoured. Also ran `openspec validate add-usb-dual-protocol
      --strict` since 7.3 touched it: it fails, but on a pre-existing `specs/` delta
      issue (`console-cli/spec.md`'s MODIFIED block omits two scenarios) unrelated to
      anything edited here — not touched, out of this change's scope
