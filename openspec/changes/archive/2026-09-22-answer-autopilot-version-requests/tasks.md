Markers: **`[board]`** the assembled board must be attached.

## 1. Implementation

- [x] 1.1 Add `LinkMsgKind::AutopilotVersion` to `include/LinkMsg.h`. No new union member —
  confirm `sizeof(LinkMsg)` and its `static_assert` are unchanged after the build (design.md
  Decision 1).
  **Done.** `sizeof(LinkMsg)` measured at 64 (compiled a standalone TU including the header),
  unchanged; the `static_assert` still holds.
- [x] 1.2 Add `case LinkMsgKind::AutopilotVersion:` to `mavlinkPack()` in `src/mavlink.cpp`,
  calling `mavlink_msg_autopilot_version_pack_chan()` with the identity triple already used
  by every other case, `capabilities = MAV_PROTOCOL_CAPABILITY_MAVLINK2`, and every other
  field `0` (design.md Decisions 1 and 2). Verify by reading the diff against
  `LinkMsgKind::Heartbeat`'s case: same shape, no fields read from `intent`.
  **Done, with a discovery along the way (design.md Decision 1a): `mavlink_msg_autopilot_version_pack_chan()`
  and `MAV_PROTOCOL_CAPABILITY_MAVLINK2` are not reachable from `<MAVLink.h>` as this
  project includes it -- the vendored common dialect never got `AUTOPILOT_VERSION`'s
  per-message header after it moved into `standard.xml`. Fixed by including
  `mavlink/standard/mavlink_msg_autopilot_version.h` directly (narrow, no redefinition
  risk) and a local `kCapabilityMavlink2 = 8192` constant, documented in place, rather than
  pulling in the whole `standard.h` (which would redefine HEARTBEAT and fail to build).
  No behaviour or spec change, only how the already-decided value is reached.**
- [x] 1.3 Add a `sendAutopilotVersion(uint8_t port)` static function following the existing
  `sendHeartbeat`/`sendCommandAck` shape (`LinkMsg intent{.kind = ...}`,
  `xQueueSend(linkPorts[port].writeQueue, &intent, 0)`), and a case in `TaskMavlink`'s
  `COMMAND_LONG` switch for `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`: call
  `sendAutopilotVersion(port)`, then `sendCommandAck(port, command.command,
  MAV_RESULT_ACCEPTED)` (design.md Decision 3). Verify with `pio run`.
  **Done.** `pio run` succeeds; see 2.1 for the headroom figure.

## 2. Build and static checks

- [x] 2.1 `pio run` and `pio run -e libs` succeed. Read the linker's RAM headroom figure and
  record it here — design.md predicts no `.bss` change, so any movement is worth explaining
  rather than assuming away.
  **Both SUCCESS. Headroom is 3180 B in both environments — unchanged from the baseline
  `size-the-log-ring-and-batch-its-flushes` left (3180 B), confirming design.md's zero-`.bss`
  prediction exactly.**
- [x] 2.2 `pio check` reports no new findings against the current baseline (2 LOW as of
  `size-the-log-ring-and-batch-its-flushes`'s own last measurement — re-read the live
  baseline, do not trust that figure without checking).
  **Confirmed: 2 LOW, unchanged** — `lib/Battery/Battery.cpp:28` (shadowFunction) and
  `src/mavlink.cpp:502` (constVariable, in the pre-existing `BatteryStatus` case). Neither
  is in the new code.

## 3. HIL verification

- [x] 3.1 Create `test/test_hil/check_capabilities.py`: send
  `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` (the same `command_long_send` shape
  `check_housekeeping.py`'s `_arm()` already uses), listen fresh rather than relying on
  `link.sample()`'s cached pre-command window (the same reason `check_clock.py` and
  `check_timesync.py` do), and assert: `AUTOPILOT_VERSION` arrives with `capabilities ==
  MAV_PROTOCOL_CAPABILITY_MAVLINK2` (and no other bit set), followed by a `COMMAND_ACK` for
  that command with `MAV_RESULT_ACCEPTED`. This is also where design.md Decision 2's claim
  gets its live confirmation, not just its source-reading one — assert the actual frame on
  the wire is MAVLink 2 (`pymavlink` exposes this per message), not only the capability bit
  the firmware reports about itself. Cover both ports the way `check_housekeeping.py` does,
  or state here why one suffices.
  **Done.** `check_capabilities.py` holds the single-port scenario (`test_capabilities_request_is_answered`):
  requests capabilities, asserts `AUTOPILOT_VERSION.capabilities == MAV_PROTOCOL_CAPABILITY_MAVLINK2`
  exactly, asserts the wire's own STX byte is `0xFD` (v2) as the live check design.md asked
  for, and asserts the `COMMAND_ACK`. The per-port isolation scenario ("requested on one port
  does not answer on the other") did **not** go here — it needs both ports open at once, so
  it follows `check_dual_link.py`'s own convention instead
  (`test_autopilot_version_goes_out_the_port_it_arrived_on`, mirroring that file's existing
  `test_reply_goes_out_the_port_it_arrived_on` for `TIMESYNC`), self-skipping without
  `HIL_UART_PORT` like every other case in that file. `run.py` discovers both by its
  `check_*.py` glob; no registration needed. Verified with `python3 -m py_compile` on both
  files — the board-run confirmation is 3.2.
- [x] 3.2 **`[board]`** `pio test` (the HIL suite) passes with the new module included,
  and the existing suite's pass/skip counts otherwise unchanged from the current baseline
  (25/8/0 as of `size-the-log-ring-and-batch-its-flushes`'s tasks.md — re-check `custom_mode`
  first per `CLAUDE.md`, and record the actual baseline read that day rather than assuming
  it still holds).
  **Done.** `custom_mode` read first: `system_status=4` (ACTIVE), `base_mode=0x84`
  (`AUTO_ENABLED` set — normal configuration), `consecutive=0`, `cumulative=2` of 10 — safe
  to reflash. `pio test`: **35 test cases: 9 skipped, 26 succeeded**, PASSED overall. Exactly
  the baseline plus this change's two additions: `check_capabilities.py`'s one case ran and
  passed (25→26); `check_dual_link.py`'s new per-port case self-skipped for the same
  documented reason its four siblings already do — no UART adapter attached (8→9 skipped).
  No other case's outcome moved.
- [x] 3.3 **`[board]`** Confirm `check_recovery.py`'s `test_observed_message_set_is_closed`
  still passes unmodified. It reads `link.sample()`'s pre-command boot-time capture
  (`run.py`'s startup call), so sending `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` in 3.1
  should not put `AUTOPILOT_VERSION` in that window — design.md assumes this; this task
  is where that assumption gets checked against the board rather than left as an assumption.
  **Confirmed: PASSED**, in the same run as 3.2. The design.md assumption held.
- [x] 3.4 **`[board]`** Read `TaskLinkWrite`'s (`UartWrite`/`UsbWrite`) stack high-water mark
  off the housekeeping stream before and after exercising 3.1, and record both here. Design.md
  Decision 1's local arrays for the custom-version fields are the only new stack cost on that
  task's frame; confirm the margin absorbs it rather than assuming a few bytes are free.
  **Done, via a direct `pymavlink` session (arm 252 at the 1000 ms floor, read a full cycle).**
  Before: `UartWrite=131`, `UsbWrite=130` words free. After 15 `AUTOPILOT_VERSION` requests
  (same session as 3.5): `UartWrite=131`, `UsbWrite=130` — **unchanged**. The new case's
  local arrays did not deepen the high-water mark at all; whatever already set it (boot-time
  activity) was already deeper than this adds.
- [x] 3.5 **`[board]`** Exercise `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` back-to-back with
  the existing periodic schedule under load (e.g. with housekeeping armed at its 1000 ms
  floor, so all four periodic entries plus this reply are contending for the same write
  queue) and confirm no `COMMAND_ACK` or `AUTOPILOT_VERSION` is dropped — design.md
  Decision 4's six-item worst-case bound, checked on the board rather than only on paper.
  **Done. 15/15 requests got both `AUTOPILOT_VERSION` and `COMMAND_ACK`** (300 ms apart,
  housekeeping armed at the 1000 ms floor throughout, so `HEARTBEAT`/`SYSTEM_TIME`/
  `BATTERY_STATUS`/`NAMED_VALUE_INT` were all live at once) — zero drops. Confirms the
  six-item worst-case bound on the real board, not just on paper.

## 4. Documentation and the backlog

- [x] 4.1 Update `ARCHITECTURE.md` §5.1: `COMMAND_LONG`'s list of "deliberately empty"
  reserved slots no longer includes `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`; state what it
  now does, next to the existing `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` /
  `MAV_CMD_SET_MESSAGE_INTERVAL` sentence.
  **Done.** Also removed `COMMAND_LONG` itself from the "deliberately empty" list it was
  already sitting in inaccurately (it has held two live sub-commands since before this
  change) rather than leaving that drift while adding a third — in scope per this change's
  own `proposal.md` Impact section.
- [x] 4.2 Edit `TODO.md`'s *"Answer the GCS messages that are ignored today"* entry in the
  proposing commit: remove the `AUTOPILOT_VERSION` bullet from its "To decide" list and note
  it is answered by this change (name the change id), leaving the entry itself in place —
  the command-support decisions, `REQUEST_DATA_STREAM`/telemetry-rate question, and the
  parameter-protocol cross-reference are still open and belong to nobody yet.
  **Done.** Grepped `TODO.md` for other `AUTOPILOT_VERSION` mentions afterward — none, so
  nothing else dangles.
