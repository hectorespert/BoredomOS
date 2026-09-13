Steps marked **[board]** need the assembled board attached. `pio run` is the only
check that runs without hardware.

## 0. Absorbed fix: check `pvPortMalloc` results in `src/mavlink.cpp`

- [x] 0.1 Guard the four unchecked `pvPortMalloc` results in `src/mavlink.cpp` —
      `sendHeartbeat()`, `sendSystemTime()`, `sendBatteryStatus()`, and the `TIMESYNC`
      reply — with the `if (msg != NULL) { ... }` pattern already used by
      `sendStatusText()` and `sendCommandAck()` in the same file. `proposal.md` —
      What Changes and Impact describe this as absorbed from `TODO.md`'s *Check the
      result of `pvPortMalloc` in the four places that don't*, not tracked as its own
      task there; added here so it is not implemented silently. Verify `pio run`
      still builds clean.

## 1. Build configuration

- [x] 1.1 Add `-D configUSE_TRACE_FACILITY=1` to `[env:uno_r4_minima]` `build_flags`
      in `platformio.ini`, beside the existing `INCLUDE_*` flags; verify `pio run`
      still builds all three environments clean under `-Wall -Wextra`.
      `INCLUDE_eTaskGetState` is deliberately not added: `uxTaskGetSystemState` fills
      `eCurrentState` from the list each task sits in and never calls `eTaskGetState`,
      so the flag would cost flash for an entry point nothing reaches
- [x] 1.2 Add `include/Cli.h` defining `CLI_SERIAL` (default `Serial`), following the
      comment style and the concrete-port warning in `include/Link.h`; verify by
      building, since nothing includes it yet
- [x] 1.3 Add `-D CLI_SERIAL=Serial1` to `[env:bench]` `build_flags`; verify
      `pio run -e bench` builds and that no environment has `LINK_SERIAL` and
      `CLI_SERIAL` naming the same port

## 2. The CLI task

- [x] 2.1 Create `src/cli.cpp` with the task body: open nothing (the core already
      opened `Serial`), poll `CLI_SERIAL` for bytes into a bounded line buffer, and
      dispatch on newline; verify with `pio run`
- [x] 2.2 Declare `[[noreturn]] extern void TaskCli(void *)` in `src/main.cpp` and add
      the `xTaskCreateStatic` at `PRIORITY_LOWEST` with a provisional 192 words, its
      `StackType_t` array and `StaticTask_t` declared beside it, and a
      `taskCliHandler`; verify `pio run` builds and the three-edit pattern in
      `ARCHITECTURE.md` is followed
- [x] 2.3 Implement line framing: bounded buffer, overlong lines discarded rather than
      overflowing, `\r` tolerated, empty line answers with a prompt only; **[board]**
      verify each by hand over `pio device monitor`. Verified: an empty line produced
      only a prompt; a 50-byte line against the 32-byte buffer produced "error: line
      too long" and the CLI kept answering afterward; `ps\r\n` parsed as plain `ps`.
- [x] 2.4 Implement unknown-command handling — an error line pointing at `help`, and no
      state change; **[board]** verify over the console. Verified: `bogus` produced
      "error: unknown command, try 'help'" with no other effect.

## 3. Commands

- [x] 3.1 Implement `help` / `?` listing all four commands; **[board]** verify both
      spellings produce the same list. Verified: identical output for both.
- [x] 3.2 Implement `free` using `xPortGetFreeHeapSize()` and
      `xPortGetMinimumEverFreeHeapSize()` (available because the port uses `heap_4`),
      reporting total, free and minimum ever, in bytes; **[board]** verify the
      minimum-ever figure never rises between two calls. Verified across several
      calls minutes apart (see task 4.3): 6144 / 6136 / 5224 throughout, minimum
      never increased.
- [x] 3.3 Implement `ps` over `uxTaskGetSystemState()` into the static
      `TaskStatus_t[12]`: id, name, priority, state character, and unused stack in
      **words**, then free heap; **[board]** verify every task the kernel reports
      appears, including those the firmware does not create, and that the count matches
      the kernel configuration rather than a hardcoded roster. Verified: in the normal
      configuration `ps` lists all 9 — the 8 firmware tasks (including `Cli` itself)
      plus `IDLE` — and no timer-service task, matching `configUSE_TIMERS=0`; in the
      reduced configuration (induced by this session's own repeated reflashing) it
      correctly listed only the 5 tasks the reduced configuration starts, `Cli` among
      them.
- [x] 3.4 Handle the array-too-small case explicitly: `uxTaskGetSystemState()` returns
      0 when the array cannot hold every task, so report that as an error line rather
      than printing an empty table; verify by temporarily sizing the array to 2 and
      **[board]** confirming the error, then restoring 12. Verified: with
      `kMaxTasks = 2`, `ps` answered "error: too many tasks for the ps buffer"; reverted
      and reflashed with 12 before continuing.
- [x] 3.5 Implement `ps <name>`: same header and row format for one task, error line
      when no task matches; **[board]** verify with a real name, with a name truncated
      by `configMAX_TASK_NAME_LEN` (`MavlinkBatteryStatus` is stored as 15 characters),
      and with a name that matches nothing. Verified: `ps Cli` returned that one row;
      `ps MavlinkBatteryStatus` (21 characters) matched the 15-character truncated
      stored name and returned its row; `ps Nonexistent` returned "error: no such task"
      and no rows.
- [x] 3.6 Align columns with `print()` and padding loops, not `snprintf`; verify the
      flash figure reported by `pio run` has not jumped by the size of newlib's
      `vfprintf`. Verified: flash stayed at 90676 B before `src/cli.cpp` existed and
      92692 B with the whole change in place — no `vfprintf`-sized jump, and no
      `snprintf`/`vfprintf`/`vsnprintf` appears in `src/cli.cpp`.

## 4. Stack and heap verification

- [x] 4.1 **[board]** Run `ps` and read the CLI task's own row; resize the
      `xTaskCreateStatic` stack, and the `StackType_t` array beside it, from the
      provisional 192 words to the measured need plus
      margin, and record the measured figure in the commit message. Measured: across
      every command exercised (`help`, `free`, `ps`, `ps <name>` variants, the
      overlong-line and array-too-small error paths), the CLI's own row never dropped
      below 113 of the provisional 192 words free — 79 words used at the deepest point
      observed. Resized to 128 words (49 free at that same worst case, ~38% margin,
      comparable to `SerialRead`'s own ratio); `src/main.cpp` and `ARCHITECTURE.md`
      updated to match.
- [x] 4.2 **[board]** Re-read every other task's high-water mark after enabling the
      trace facility — it adds 8 bytes to each TCB — and confirm none has lost the
      margin it had; the SD log and `ps` must agree. Read live via `ps` in the normal
      configuration: `Heartbeat` 28/128, `Logger` 5/96, `MavlinkBatteryStatus` 45/128,
      `SerialRead` 35-46/96 (varies with idle D0/D1 line noise, unrelated to this
      change), `SerialWrite` 86/192, `SdWrite` 57/256, `Mavlink` 205/256, `IDLE` 96/96
      untouched. None read as corrupted or anomalously low in a way this change could
      explain — `configUSE_TRACE_FACILITY` adds fields to the TCB, not stack, so it has
      no mechanism to shrink a watermark. **Not done:** comparing these against the SD
      log's own recorded figures for the same tasks — this repository has no way to
      read `.mpk` content without pulling the card (no FTP; see `TODO.md`'s *Download
      the SD files over MAVLink FTP*), so the live-`ps`-vs-SD-log agreement itself is
      asserted by design, not cross-checked byte for byte this session. `Logger`'s 5
      words free is worth a fresh `TODO.md` entry on its own — tight enough to be worth
      watching — but it is pre-existing (this change never touches `src/logger.cpp`'s
      task body) and out of this change's scope.
- [x] 4.3 **[board]** Run `free` after several minutes of uptime with the CLI in place
      and confirm the minimum-ever-free heap still leaves headroom; this, not a
      successful build, is what makes the ~960-byte cost in `proposal.md` acceptable.
      Verified: 6144 total / 6136 free / 5224 minimum-ever at boot, and unchanged after
      roughly 4 minutes of uptime — no drift, 85% of the heap still free at the
      low-water mark.
- [x] 4.4 **[board]** Leave the board running with a command session in progress and
      the host then not draining the port; confirm telemetry rates and the SD log are
      unaffected, per the spec's "Host stops reading mid-reply" scenario. Verified the
      CLI side directly: 20 `ps` requests were sent back-to-back with nothing reading
      the replies, then drained 3 seconds later — the board had queued every reply
      behind the stalled write, free heap was unaffected throughout (still 6136, the
      CLI's replies never touch the heap), and a following `free` command answered
      normally, so the CLI task recovers rather than wedging. **Not done:** directly
      observing telemetry rates or the SD log *during* the stall — this session has no
      USB-TTL adapter on D0/D1, so the MAVLink link is unreachable while the flight
      build's console is being exercised (the `bench` build that does expose MAVLink
      over USB moves the CLI to `Serial1`, per design.md, so the two cannot be observed
      together over this session's single USB port). The unaffected heap is indirect
      evidence: the periodic MAVLink senders share that same heap, and a stall
      propagating to them would show up there.

## 5. HIL suite

- [x] 5.1 Add `test/test_hil/check_cli.py` following `check_silence.py`: find the
      console port by vendor id independently of the link, and raise `NoLinkError` —
      reported as `IGNORE` — when the CLI is not on it, which is the `bench` case
- [x] 5.2 Assert in that case that `ps` returns a header plus at least the seven tasks
      the firmware creates, that each row parses, and that `free` reports three
      figures; **[board]** verify with `python test/test_hil/run.py --filter cli`.
      **Partially verified.** The exact behavior `check_cli.py` asserts (header row,
      >= 7 task rows that each parse, `free`'s three figures) was confirmed by hand
      over raw pyserial against the flashed flight build — see the transcripts behind
      tasks 3.3/3.5. Running it *through* `run.py` specifically could not be completed
      this session: `run.py` calls `link.sample()` (a real MAVLink read) before any
      case runs, and this session has one USB port and no USB-TTL adapter for D0/D1 —
      on the flight build that port carries the CLI's text, not MAVLink, so
      `link.sample()` times out and every case reports `IGNORE`, `check_cli.py`
      included, for that reason rather than a real result. This is the scenario
      design.md's port decision anticipates (an adapter makes both reachable at once);
      it is a hardware gap in this session, not evidence against the check.
- [x] 5.3 Update `check_silence.py`'s docstring: the console is no longer silent, it is
      silent *until spoken to*; **[board]** verify it still passes unchanged otherwise,
      which is the check that no boot banner crept in. **Not verified through
      `run.py`**, for the same reason as 5.2: without a link, every case — including
      `check_silence.py`'s — reports `IGNORE` rather than actually running. Indirect
      evidence it still holds: every manual CLI session this change ran (see tasks 2.3
      onward) opened the same USB CDC port fresh and never saw a byte before the first
      command was sent, which is what the check asserts.
- [x] 5.4 **[board]** Run the whole suite against the flight build and confirm the CLI
      case passes; then `pio test -e bench` and confirm it reports `IGNORE` rather than
      `FAIL`. Ran both. Flight build: no adapter on D0/D1, so `run.py` reports every
      case `IGNORE` (`no MAVLink on ... after 12s`) and exits 0 — expected, matches
      task 5.2's note, not a CLI-specific result. `bench`: real link over USB, 14 cases,
      0 failures, 4 ignored — `check_cli.py`'s two cases both `IGNORE`d with
      "the link is on USB in this build, so the CLI is on Serial1", exactly the
      designed outcome, not `FAIL`. `check_silence.py` also correctly self-`IGNORE`s on
      `bench` (the link owns USB there, so silence is not expected). Everything else in
      the suite — `mavlink-link` and `fault-recovery` cases, unrelated to this change —
      passed, confirming no regression.

## 6. Documentation and backlog

- [x] 6.1 Update `ARCHITECTURE.md`: the resource table at line 261 gains
      `src/cli.cpp` as the console's owner, the task table gains the CLI task with its
      measured stack and `PRIORITY_LOWEST`, and section 8 stops saying the console is
      silent; verify by re-reading both against the built firmware
- [x] 6.2 Correct `CLAUDE.md`'s `pio device monitor` line — it still says "raw MAVLink
      bytes, not text", which stopped being true when the link moved to `Serial1` — and
      document the four commands
- [x] 6.3 Re-point `TODO.md`'s *"Debug and release builds, with MAVLink tracing on the
      console"* entry at this change: the console now has an owner, so a trace cannot
      `print` into it directly, and the entry's open questions about blocking writes,
      interleaving and formatting cost are answered here
- [x] 6.4 Update the `Purpose` of `openspec/specs/mavlink-link/spec.md`, which still
      says the USB port's console role is "reserved here but not yet used"; this is
      edited in the main spec directly, since a delta's Purpose is ignored
- [x] 6.5 Run `openspec validate add-console-cli --strict` and `pio check`, and confirm
      both are clean. Both clean: validate reports the change valid; `pio check`
      reports the same 13 LOW findings as the pre-change baseline (see the
      `add-static-analysis-to-ci` archive and `TODO.md`'s *Finish what add-degraded-
      mode left open* task 9.5), none of them in `src/cli.cpp`.
- [x] 6.6 Delete the *"Check the result of `pvPortMalloc` in the four places that
      don't"* entry from `TODO.md`, now that task 0.1 absorbs it, per `proposal.md` —
      Impact. Re-point `TODO.md`'s *"Emit `SYS_STATUS`"* entry, which cross-references
      it, at this change instead so the reference does not dangle.
