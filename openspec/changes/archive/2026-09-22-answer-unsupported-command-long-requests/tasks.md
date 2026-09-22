## 1. Implement the default COMMAND_ACK responses

- [x] 1.1 In `src/mavlink.cpp`'s `COMMAND_LONG` case (around lines 757-838),
      convert the four independent `if (command.command == ...)` blocks into
      an `if` / `else if` / `else` chain, with the final `else` calling
      `sendCommandAck(port, command.command, MAV_RESULT_UNSUPPORTED)`. Verify
      `pio run` succeeds on both environments (`uno_r4_minima`, `libs` --
      `bench` no longer exists, corrected here from the proposal's error)
      with no new warnings.
- [x] 1.2 In the `MAV_CMD_SET_MESSAGE_INTERVAL` handler, add an `else` branch
      for `(uint16_t)command.param1 != MAVLINK_MSG_ID_NAMED_VALUE_INT` that
      calls `sendCommandAck(port, command.command, MAV_RESULT_DENIED)`, and
      confirm the existing `NAMED_VALUE_INT` (252) branch and its floor/denial
      logic are unchanged. Verify `pio run` succeeds on both environments.
- [x] 1.3 Run `pio check` and compare against the current baseline (`TODO.md`,
      "Finish what add-degraded-mode left open", item 9.5 — 13 LOW findings).
      Record here whether this change adds, removes or leaves that count
      unchanged; do not let a new finding go unrecorded.

      **Result:** `pio check` today reports only 2 LOW findings total
      (`lib/Battery/Battery.cpp:28` shadowFunction,
      `src/mavlink.cpp:502` constVariable), neither touched by this change and
      neither on a line this change added. The TODO.md-recorded figure of 13
      does not match what is observed now — stale relative to today's
      `platformio.ini`/cppcheck configuration, unrelated to this change — but
      the fact this task exists to check holds either way: **this change adds
      zero new cppcheck findings.**

## 2. Keep the docs honest

- [x] 2.1 Update `ARCHITECTURE.md`'s `COMMAND_LONG` paragraph (the one
      currently reading "every other command it receives still falls through
      unanswered") to describe the `else` default and the
      `SET_MESSAGE_INTERVAL` denial added in section 1. Verify by re-reading
      the paragraph against the new code.
- [x] 2.2 Annotate `TODO.md`'s "Answer the GCS messages that are ignored
      today" entry, in the style already used there for `AUTOPILOT_VERSION`
      (`answer-autopilot-version-requests`): record that the "always answer
      something" floor for `COMMAND_LONG` is now closed by this change, and
      that the parameter protocol, `REQUEST_DATA_STREAM`/telemetry-rate
      question, and "which commands deserve real support" remain open. Do
      **not** delete the entry — only part of it is picked up here.

## 3. Add HIL coverage

- [x] 3.1 Add `test/test_hil/check_unsupported_commands.py` with
      `test_unrecognised_command_is_denied_unsupported`: send a
      `COMMAND_LONG` naming a command this firmware never checks (e.g.
      `MAV_CMD_DO_SET_MODE`), assert a `COMMAND_ACK` for that command arrives
      with `result == MAV_RESULT_UNSUPPORTED`. Model `_request`/`_wait_ack`
      on `check_capabilities.py`'s helpers. `run.py --list` confirms the case
      is discovered.
- [x] 3.2 In the same module, add
      `test_get_home_position_is_answered_unsupported`: send
      `MAV_CMD_GET_HOME_POSITION`, assert `COMMAND_ACK` /
      `MAV_RESULT_UNSUPPORTED` (covers the spec's dedicated scenario for this
      command, which was checked but never acknowledged before this change).
- [x] 3.3 In the same module, add
      `test_set_message_interval_unsupported_msgid_is_denied`: send
      `MAV_CMD_SET_MESSAGE_INTERVAL` naming a message id other than 252 (e.g.
      `HEARTBEAT`'s), assert `COMMAND_ACK` / `MAV_RESULT_DENIED`.
- [x] 3.4 **Needs the board.** Flash the firmware and run `pio test` (HIL,
      default USB target). Confirm the three new cases pass alongside the
      existing suite, and that none of the existing cases (in particular
      `check_recovery.py`'s `test_observed_message_set_is_closed`, which
      already allows `COMMAND_ACK` unconditionally) regress.

      **Result:** board at `/dev/cu.usbmodem2101`. `pio test` flashed and ran
      the full HIL suite: 38 cases, 29 executed (9 self-skip without an
      adapter/manual reset/reduced configuration), 0 failures. All three new
      cases passed
      (`test_unrecognised_command_is_denied_unsupported`,
      `test_get_home_position_is_answered_unsupported`,
      `test_set_message_interval_unsupported_msgid_is_denied`), and
      `check_recovery.py`'s `test_observed_message_set_is_closed` passed —
      no regression.
- [x] 3.5 **Needs the board and hands at the link.** Arm the housekeeping
      stream (`MAV_CMD_SET_MESSAGE_INTERVAL` on message id 252, interval
      >= 1000 ms), exercise the new branches from 3.1-3.3, and read
      `TaskMavlink`'s stack high-water mark from the `NAMED_VALUE_INT`
      stream. Record the figure and confirm it stays comfortably within the
      task's stack (`CLAUDE.md`'s rule: check the high-water mark after any
      task-body edit).

      **Result:** armed at 1000 ms, sent all three new command shapes, then
      read `Mavlink`'s `NAMED_VALUE_INT`: **125 of 384 words free**
      (`src/main.cpp`'s `xTaskCreateStatic(TaskMavlink, "Mavlink", 384, ...)`).
      Close to the 122-word reference reading recorded during
      `replace-messagepack-log-with-dataflash` (task 9.3), well within the
      ~15-word sample-to-sample variance that same record already documents
      for this task — no regression, comfortable margin. Disarmed housekeeping
      afterward, matching `check_housekeeping.py`'s convention of leaving the
      stream off for whatever runs next.

## 4. Confirm the memory and allocation invariants hold

- [x] 4.1 Run `pio run` on both environments (`uno_r4_minima`, `libs` --
      `bench` no longer exists) and confirm `scripts/ram_budget.py` reports no
      reduction in `.bss` headroom versus the pre-change build — this change
      adds no task, queue or library, so the figure should be unchanged;
      confirm rather than assume.

      **Result:** headroom is 3180 B (minimum 1024) on both environments,
      before and after this change (verified by building against
      `git stash`ed `src/mavlink.cpp`, then against the modified version) --
      unchanged, as expected.
- [x] 4.2 Confirm CI's grep of `src/` for `xTaskCreate`/`xQueueCreate` and for
      the dynamic-allocation function this project's `.bss`-only rule
      forbids still passes (no new call sites were added by this change).

      **Result:** `grep -n "xTaskCreate\|xQueueCreate\|pvPortMalloc" src/*.cpp`
      finds only the existing `xTaskCreateStatic`/`xQueueCreateStatic` calls in
      `src/main.cpp`; no dynamic-allocation call site anywhere in `src/`.
