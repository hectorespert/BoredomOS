Steps marked **[board]** need the assembled board attached. `pio run` and `pio check`
are the only checks that run without hardware.

## 1. Define the by-value outbound type

- [x] 1.1 Add `include/LinkMsg.h` declaring `LinkMsgKind` and the `LinkMsg` tagged
      union exactly as design.md's Decisions lay out (`Heartbeat`, `SystemTime`,
      `BatteryStatus`, `TimesyncReply`, `StatusText`, `CommandAck`,
      `NamedValueInt`); add a `static_assert(sizeof(LinkMsg) <= 64, ...)` so a
      future variant that grows the union fails the build instead of silently
      changing the RAM math; verify with `pio run`
- [x] 1.2 Add `void mavlinkPack(const LinkMsg &intent, mavlink_message_t *out);`
      to `include/MavlinkPack.h` (new header, declarations only — named
      `MavlinkPack.h`, not `Mavlink.h` as design.md originally sketched: this
      filesystem resolves `#include <MAVLink.h>` case-insensitively, so a
      same-named header here shadowed the library's own file and broke every
      MAVLink symbol until renamed) and its definition in `src/mavlink.cpp` —
      one `switch (intent.kind)` calling the matching `mavlink_msg_*_pack`,
      with the same three-value identity (`1`, `MAV_COMP_ID_AUTOPILOT1`,
      `MAV_TYPE_ROCKET`/`MAV_AUTOPILOT_GENERIC` per message) every existing
      `send*` function already used; verified with `pio run`

## 2. Convert the outbound path

- [x] 2.1 Rewrite `sendHeartbeat`, `sendSystemTime`, `sendBatteryStatus`,
      `sendStatusText`, `sendCommandAck` and `sendHousekeeping` in
      `src/mavlink.cpp`: each fills a `LinkMsg` value (no `pvPortMalloc`, no
      `NULL` check) and calls `xQueueSend(serialWriteQueue, &intent, 0)`
      directly, with nothing to free on failure; verified with `pio run` and by
      inspection that no `pvPortMalloc` remains in these six functions
- [x] 2.2 Convert the inline `TIMESYNC` reply inside `TaskMavlink`'s switch
      (`src/mavlink.cpp`, the `tc1 == 0` branch) the same way — fill a
      `LinkMsg{kind = TimesyncReply}` and send it, dropping the
      `pvPortMalloc`/`NULL`-check/`vPortFree` around it; verified with `pio run`
- [x] 2.3 While rewriting `sendStatusText`'s only unhandled-message call site
      (`TaskMavlink`'s `default` case), fix the pre-existing defect found
      incidentally: `"Mensaje recibido con ID desconocido: " + msg.msgid` is
      pointer arithmetic on a string literal, not concatenation, and produces
      a truncated or out-of-bounds substring rather than the intended text for
      any `msgid` at or past the literal's length. Replace it with a fixed
      string that names no dynamic value (e.g. `"Unhandled message received"`)
      rather than adding a formatting helper this task does not otherwise
      need; verified by inspection and with `pio run` — board confirmation that
      the reply text now reads correctly is task 7.4
- [x] 2.4 Change `serialWriteQueue`'s element type from `mavlink_message_t*` to
      `LinkMsg` in `src/main.cpp`: `xQueueCreateStatic`'s item size and
      `serialWriteQueueStorage`'s declared size (`5 * sizeof(LinkMsg)`); verified
      with `pio run` and confirmed the new storage size against `sizeof(LinkMsg)`
      from task 1.1 (64 B exactly, backed out of the measured `.bss` delta)
- [x] 2.5 Rewrite `TaskSerialWrite` in `src/serial.cpp`: receive a `LinkMsg` by
      value, call `mavlinkPack(intent, &msg)` into a local `mavlink_message_t`,
      then `mavlink_msg_to_send_buffer` and write exactly as today — no
      `vPortFree`, since nothing was allocated; verified with `pio run`

## 3. Convert the inbound path

- [x] 3.1 Change `serialReadQueue`'s element type from `mavlink_message_t*` to
      `mavlink_message_t` in `src/main.cpp`: `xQueueCreateStatic`'s item size
      and `serialReadQueueStorage`'s declared size (`8 * sizeof(mavlink_message_t)`);
      verified with `pio run`
- [x] 3.2 Rewrite `TaskSerialRead` in `src/serial.cpp`: on a complete parse,
      `xQueueSend(serialReadQueue, &msg_to_read, 0)` the file-static message by
      value instead of `pvPortMalloc`-ing a copy; verified with `pio run` and by
      inspection that no `pvPortMalloc` remains in `src/serial.cpp`
- [x] 3.3 Update `TaskMavlink`'s receive loop in `src/mavlink.cpp`:
      `xQueueReceive` into a local `mavlink_message_t msg` (not a pointer),
      change every `msg->` to `msg.` in the switch and its decode calls, and
      drop the trailing `vPortFree(msg)`; verified with `pio run` and by
      inspection that no `pvPortMalloc`/`vPortFree` remains in
      `src/mavlink.cpp`

## 4. Shrink the FreeRTOS heap

- [x] 4.1 **[board or clean build]** Build with the provisional
      `configTOTAL_HEAP_SIZE=0x200` from design.md and run `pio run`; confirmed
      the build succeeds and read the new `RAM budget` block
      `scripts/ram_budget.py` prints: 26056 B committed, 6712 B headroom,
      exactly matching design.md's projection
- [x] 4.2 **[board]** With that build flashed, exercise `sdWriteQueue` under
      normal operation for several minutes (SD card present, logging at 1 Hz)
      and read `free` (still available — the CLI is not removed by this
      change) for the minimum-ever-free heap; if it is not comfortably above
      zero, raise `configTOTAL_HEAP_SIZE` in `platformio.ini` and repeat this
      task with the new value. The board had fallen into, and stayed in, the
      reduced configuration (this session's stack-overflow crash's watchdog-
      reset loop pushed the cumulative fault counter over threshold — waiting
      out the 5-minute stability window and forcing a reset was not enough,
      confirming it was the cumulative counter, not the consecutive one).
      Recovered it with `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` (246) over the
      `bench` build's USB MAVLink link — already implemented
      (`add-degraded-mode`), it clears both counters via
      `Recovery::reinitialise()` before resetting; confirmed via the
      heartbeat's `system_status` flipping from `MAV_STATE_CRITICAL` to
      `MAV_STATE_ACTIVE` and housekeeping listing all 8 tasks. Reflashed the
      flight build and soaked for ~3.5 minutes under real 1 Hz SD logging:
      **heap free 496/512, minimum-ever-free 448/512, unchanged before and
      after the soak** — stable, no leak, comfortable margin. `TODO.md`'s
      claim that no ground command exists for this was wrong; corrected in
      this commit.
- [x] 4.3 Record the final chosen `configTOTAL_HEAP_SIZE` (`0x200`, confirmed by
      a clean build in task 4.1 — dynamic confirmation is task 4.2, still open)
      and the resulting committed/headroom figures (26056 B / 6712 B) in
      `ARCHITECTURE.md` (task 6.1) and `design.md`; the commit message carries
      the same figures when this is committed

## 5. CI guard

- [x] 5.1 Extend `.github/workflows/main.yml`'s existing grep step to also
      forbid `pvPortMalloc` in `src/serial.cpp` and `src/mavlink.cpp`
      specifically — not all of `src/`, since `src/logger.cpp`/`src/sdwrite.cpp`
      still legitimately use it for `sdWriteQueue` (design.md's corrected CI
      section, itself a correction of this task's original wording, which
      assumed no `pvPortMalloc` remained anywhere in `src/`); verified by
      temporarily reintroducing one `pvPortMalloc` call in `src/mavlink.cpp`
      and confirming the grep would match, then removing the temporary
      reintroduction

## 6. Documentation

- [x] 6.1 Update `ARCHITECTURE.md` §4 ("Queue memory ownership protocol"):
      replaced the "queues carry heap pointers, never values" framing with what
      this change makes true — `serialReadQueue`/`serialWriteQueue` are
      by-value now, only `sdWriteQueue` still uses the heap-pointer protocol —
      updated the queue table and every other RAM figure in the document
      (§2's diagram, §7, §8) with the measured figures from task 4.3, and
      updated §5.1's description of `TaskSerialRead`/`TaskSerialWrite`/
      `TaskMavlink` to match the by-value queues
- [x] 6.2 Update `CLAUDE.md`'s "Queues carry heap pointers, never values"
      convention bullet: states which queue still follows it (`sdWriteQueue`
      only) and which now carry values, with the one-line reason (291-byte
      items made by-value storage prohibitively expensive; these items no
      longer are)
- [x] 6.3 Delete the `TODO.md` entry "Queue the message intent by value instead
      of a packed `mavlink_message_t`" that this change implements; grep
      `TODO.md` and `ARCHITECTURE.md` for any other entry cross-referencing it
      by name and re-point those references at this change's id
      (`queue-mavlink-messages-by-value`) — done when this change was
      scaffolded (its entry was replaced by the CLI-removal backlog entry that
      depends on this change), confirmed clean by grep with no remaining
      references

## 7. Verification on the board

- [x] 7.1 **[board]** Run `pio test` (the HIL suite) unmodified and confirm
      every case still passes — this change makes no on-wire behavior change,
      so the existing suite is the regression check. Ran `pio test -e bench`:
      16 passed, 5 skipped (`check_cli.py`'s two cases and
      `check_silence.py`'s one self-skip on `bench` by design; the manual
      reset case in `check_housekeeping.py` needs a human at the button;
      `test_heartbeat_reports_operational` self-skips because the board is in
      the reduced configuration — see task 4.2), 1 failed
      (`test_battery_status_every_2s`). That failure is the pre-existing gap
      `TODO.md` already documents under "`check_telemetry.py`'s
      `test_battery_status_every_2s` assumes the normal configuration" — it
      doesn't yet self-skip when `BATTERY_STATUS` is correctly disabled in the
      reduced configuration this board is stuck in, and predates this change.
      No case failed that this change's diff touches.
- [x] 7.2 **[board]** Read `TaskSerialRead`'s, `TaskSerialWrite`'s and
      `TaskMavlink`'s stack high-water marks via `ps` (or the SD log) before
      and after this change; confirm none lost its margin, in particular
      `TaskSerialWrite` and `TaskMavlink`, whose locals now include a
      `mavlink_message_t` neither carried before (design.md's Risks).
      `TaskSerialWrite` **did** overflow at the originally-planned 192 words —
      caught on the board (slow 2s-on/2s-off LED, confirmed with the user),
      grown to 384, now measures 146 of 384 words free. `TaskMavlink` measured
      41 of 256 words free — thinner than the ~127 `add-mavlink-housekeeping-
      telemetry` recorded, and before exercising `COMMAND_LONG` at all — grown
      to 384 proactively, now measures 169 of 384 free. `TaskSerialRead`
      unchanged at 45 of 96 free (no new stack cost). Both stack-size changes
      are recorded in `src/main.cpp` with the measurements in comments.
- [x] 7.3 **[board]** With `mavproxy.py` attached, confirm `HEARTBEAT`,
      `SYSTEM_TIME` and `BATTERY_STATUS` still leave at their documented rates
      and identity, and that a `TIMESYNC` round-trip and an armed housekeeping
      stream (`MAV_CMD_SET_MESSAGE_INTERVAL` on id 252) still behave exactly as
      before this change. Confirmed via `pio test -e bench` (task 7.1):
      `HEARTBEAT`/`SYSTEM_TIME` at 1 Hz, identity, vehicle type, no garbage on
      the link, `TIMESYNC` answered, and housekeeping's full arm/deny/disable/
      default-rate cycle including the reduced-configuration case all passed.
      `BATTERY_STATUS`'s rate could not be confirmed — same reduced-
      configuration blocker as task 4.2, where it is correctly disabled by
      design rather than broken.
- [x] 7.4 **[board]** Send a message with an unhandled `msgid` (anything not in
      `TaskMavlink`'s switch) and confirm the `STATUSTEXT` reply reads the
      fixed text from task 2.3, not truncated or garbage content — this is the
      regression check for the incidental fix. Sent `PING` (msgid 4, not one
      of `TaskMavlink`'s handled ids) over the `bench` build's USB MAVLink
      link; received `STATUSTEXT` severity 4 (`MAV_SEVERITY_WARNING`), text
      exactly `"Unhandled message received"` — no truncation, no garbage.
- [x] 7.5 Run `openspec validate queue-mavlink-messages-by-value --strict` and
      `pio check` and confirm both are clean — validate passes (only the
      expected `skip_specs` info note), `pio check` reports 0 HIGH/MEDIUM
      findings (6 LOW, all pre-existing except one relocated verbatim from
      `sendBatteryStatus` into `mavlinkPack`, not new)
