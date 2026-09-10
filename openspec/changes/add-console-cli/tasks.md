Steps marked **[board]** need the assembled board attached. `pio run` is the only
check that runs without hardware.

## 1. Build configuration

- [ ] 1.1 Add `-D configUSE_TRACE_FACILITY=1` to `[env:uno_r4_minima]` `build_flags`
      in `platformio.ini`, beside the existing `INCLUDE_*` flags; verify `pio run`
      still builds all three environments clean under `-Wall -Wextra`.
      `INCLUDE_eTaskGetState` is deliberately not added: `uxTaskGetSystemState` fills
      `eCurrentState` from the list each task sits in and never calls `eTaskGetState`,
      so the flag would cost flash for an entry point nothing reaches
- [ ] 1.2 Add `include/Cli.h` defining `CLI_SERIAL` (default `Serial`), following the
      comment style and the concrete-port warning in `include/Link.h`; verify by
      building, since nothing includes it yet
- [ ] 1.3 Add `-D CLI_SERIAL=Serial1` to `[env:bench]` `build_flags`; verify
      `pio run -e bench` builds and that no environment has `LINK_SERIAL` and
      `CLI_SERIAL` naming the same port

## 2. The CLI task

- [ ] 2.1 Create `src/cli.cpp` with the task body: open nothing (the core already
      opened `Serial`), poll `CLI_SERIAL` for bytes into a bounded line buffer, and
      dispatch on newline; verify with `pio run`
- [ ] 2.2 Declare `[[noreturn]] extern void TaskCli(void *)` in `src/main.cpp` and add
      the `xTaskCreate` at `PRIORITY_LOWEST` with a provisional 192 words and a
      `taskCliHandler`; verify `pio run` builds and the three-edit pattern in
      `ARCHITECTURE.md` is followed
- [ ] 2.3 Implement line framing: bounded buffer, overlong lines discarded rather than
      overflowing, `\r` tolerated, empty line answers with a prompt only; **[board]**
      verify each by hand over `pio device monitor`
- [ ] 2.4 Implement unknown-command handling — an error line pointing at `help`, and no
      state change; **[board]** verify over the console

## 3. Commands

- [ ] 3.1 Implement `help` / `?` listing all four commands; **[board]** verify both
      spellings produce the same list
- [ ] 3.2 Implement `free` using `xPortGetFreeHeapSize()` and
      `xPortGetMinimumEverFreeHeapSize()` (available because the port uses `heap_4`),
      reporting total, free and minimum ever, in bytes; **[board]** verify the
      minimum-ever figure never rises between two calls
- [ ] 3.3 Implement `ps` over `uxTaskGetSystemState()` into the static
      `TaskStatus_t[12]`: id, name, priority, state character, and unused stack in
      **words**, then free heap; **[board]** verify every task the kernel reports
      appears, including those the firmware does not create, and that the count matches
      the kernel configuration rather than a hardcoded roster
- [ ] 3.4 Handle the array-too-small case explicitly: `uxTaskGetSystemState()` returns
      0 when the array cannot hold every task, so report that as an error line rather
      than printing an empty table; verify by temporarily sizing the array to 2 and
      **[board]** confirming the error, then restoring 12
- [ ] 3.5 Implement `ps <name>`: same header and row format for one task, error line
      when no task matches; **[board]** verify with a real name, with a name truncated
      by `configMAX_TASK_NAME_LEN` (`MavlinkBatteryStatus` is stored as 15 characters),
      and with a name that matches nothing
- [ ] 3.6 Align columns with `print()` and padding loops, not `snprintf`; verify the
      flash figure reported by `pio run` has not jumped by the size of newlib's
      `vfprintf`

## 4. Stack and heap verification

- [ ] 4.1 **[board]** Run `ps` and read the CLI task's own row; resize the
      `xTaskCreate` stack from the provisional 192 words to the measured need plus
      margin, and record the measured figure in the commit message
- [ ] 4.2 **[board]** Re-read every other task's high-water mark after enabling the
      trace facility — it adds 8 bytes to each TCB — and confirm none has lost the
      margin it had; the SD log and `ps` must agree
- [ ] 4.3 **[board]** Run `free` after several minutes of uptime with the CLI in place
      and confirm the minimum-ever-free heap still leaves headroom; this, not a
      successful build, is what makes the ~960-byte cost in `proposal.md` acceptable
- [ ] 4.4 **[board]** Leave the board running with a command session in progress and
      the host then not draining the port; confirm telemetry rates and the SD log are
      unaffected, per the spec's "Host stops reading mid-reply" scenario

## 5. HIL suite

- [ ] 5.1 Add `test/test_hil/check_cli.py` following `check_silence.py`: find the
      console port by vendor id independently of the link, and raise `NoLinkError` —
      reported as `IGNORE` — when the CLI is not on it, which is the `bench` case
- [ ] 5.2 Assert in that case that `ps` returns a header plus at least the seven tasks
      the firmware creates, that each row parses, and that `free` reports three
      figures; **[board]** verify with `python test/test_hil/run.py --filter cli`
- [ ] 5.3 Update `check_silence.py`'s docstring: the console is no longer silent, it is
      silent *until spoken to*; **[board]** verify it still passes unchanged otherwise,
      which is the check that no boot banner crept in
- [ ] 5.4 **[board]** Run the whole suite against the flight build and confirm the CLI
      case passes; then `pio test -e bench` and confirm it reports `IGNORE` rather than
      `FAIL`

## 6. Documentation and backlog

- [ ] 6.1 Update `ARCHITECTURE.md`: the resource table at line 261 gains
      `src/cli.cpp` as the console's owner, the task table gains the CLI task with its
      measured stack and `PRIORITY_LOWEST`, and section 8 stops saying the console is
      silent; verify by re-reading both against the built firmware
- [ ] 6.2 Correct `CLAUDE.md`'s `pio device monitor` line — it still says "raw MAVLink
      bytes, not text", which stopped being true when the link moved to `Serial1` — and
      document the four commands
- [ ] 6.3 Re-point `TODO.md`'s *"Debug and release builds, with MAVLink tracing on the
      console"* entry at this change: the console now has an owner, so a trace cannot
      `print` into it directly, and the entry's open questions about blocking writes,
      interleaving and formatting cost are answered here
- [ ] 6.4 Update the `Purpose` of `openspec/specs/mavlink-link/spec.md`, which still
      says the USB port's console role is "reserved here but not yet used"; this is
      edited in the main spec directly, since a delta's Purpose is ignored
- [ ] 6.5 Run `openspec validate add-console-cli --strict` and `pio check`, and confirm
      both are clean
